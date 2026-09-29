-- R4a / R4b ai-save (docs\dead-air\ai\PLAN.md 7.3): the wall, suppression and search, then the beliefs across a save.
-- R4a (default): at t=0 the level's own creatures within 300 m go offline for the run (again every 0.5 s for whatever
-- walks in). An invulnerable hostile loner T stands 25 m in front of bandit squad D (3) and a merc pair D2
-- (merc_sim_squad_advanced cut to two: mercs are neutral to bandits, hostile to loners, and their radios vary), all in
-- sight of him. At t=8 s T steps 3-5 m sideways behind the wall a ray found: for 10 s every shot is logged with the
-- shooter's sight of T and how long it has been gone, and every hit on T the same way (P2). At t=20 s T moves at least
-- 95 m away out of every sight line of the shooters and of the ground they search (the widest ring is 60 m around the
-- last known position, a spot is cleared by sight within 25 m): nobody finds him, so the search runs to its give-up.
-- The shortest search is the novice bandits': search_s 30 x 1.3 (a sworn enemy) + a 4-12 s stagger after the later of
-- the search start (~13-17 s) and the last sighting (<= 20 s), so every bandit gives up by ~71 s and the squad's
-- give-up spread (r_giveup_spread_ms, B7) is counted; the mercs (search_s >= 150) still search at the save. At t=78 s
-- every NPC prints its belief on T and its radio, at t=80 s the game is saved as qa_ai_mid (by day). At t=82 s, the
-- save captured, the night muzzle-flash trial of B1 (see "After the save"): F fires at 83.3 s, done 86.5, quit ~89.5.
-- The actor is a bystander (ai_ignore_actor 1, g_god 1).
-- R4b (-Globals 'qa_ai_phase = "b"', -Save qa_ai_mid): the same NPCs (their roles ride in the save's script data)
-- print their belief on T and their radio right after the load (P8).
--
--   tools\qa\Run-WaterQa.ps1 -Name ai-save-a -Scenario ai_save -Save fogqa -Seconds 150 `
--       -ExtraArgs "-force_flushlog -always_active" -Out "D:\Games\Dead Air\_work\ai-npc\qa"
--   tools\qa\Run-WaterQa.ps1 -Name ai-save-b -Scenario ai_save -Save qa_ai_mid -Seconds 60 `
--       -Globals 'qa_ai_phase = "b"' -ExtraArgs "-force_flushlog -always_active" `
--       -Out "D:\Games\Dead Air\_work\ai-npc\qa"
--
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

-- The spot passing `must` (bearings in 5 degree steps) that scores highest, with its score.
local function best_spot(center_vid, radii, start_deg, must, score)
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
	if best then return best[1], best[2], best[3], best_score end
end

-- find_spot with a fallback: when no spot passes `test`, the spot passing `must` that scores highest (the number of
-- sight lines it hides from). The caller logs the relaxed placement; the parser reads the sight flips that follow.
local function find_spot_or_best(center_vid, radii, start_deg, test, must, score)
	local vid, p, deg = find_spot(center_vid, radii, start_deg, test)
	if vid then return vid, p, deg, false end
	local v, q, b, s = best_spot(center_vid, radii, start_deg, must, score)
	if v then return v, q, b, true, s end
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

------------------------------------------------------------------------------------------------------------------------
-- The level's own creatures out of the trials (the same block in ai_fair.lua and ai_save.lua)
------------------------------------------------------------------------------------------------------------------------
-- Every living stalker and monster of the level within SWEEP_RADIUS of the actor that the probe did not spawn is kept
-- offline for the run: its squad (or itself, outside a squad) gets the switch flags "may go offline, never online"
-- (alife():set_switch_offline/set_switch_online, what alun_utils.switch_offline and DA's debug toggle set), which the
-- ALife switch manager applies on its next pass over the level. Offline is where every level NPC is while the actor is
-- elsewhere, so no script meets a released object (story NPCs included): nothing is released. The flags ride in a save;
-- only R4a writes one (qa_ai_mid, a QA save). Traders (they never fight) and squads that refuse to go offline (the
-- actor's companions) are left alone and counted as kept. One pass over every ALife object runs first; afterwards,
-- every SWEEP_PERIOD_MS, the online creatures (db.storage) and every SWEEP_SQUADS_EVERY-th time the sim squads
-- (SIMBOARD) near the actor, for whatever walked in or spawned since. Lines "sweep scope=<all|all_ids|online|squads>
-- new=<n> squads=<n> singles=<n> kept=<n> online=<n> nearest=<m>" on a change and every SWEEP_HEARTBEAT_MS: online
-- counts the foreign creatures still online within the radius (0 once the switch manager has run), nearest the closest
-- (-1: none); all_ids is the first pass done id by id (the engine's iterate_objects failed).
local SWEEP_RADIUS, SWEEP_PERIOD_MS, SWEEP_SQUADS_EVERY, SWEEP_HEARTBEAT_MS = 300, 500, 4, 15000
local own_ids = {} -- everything the probe spawned that can think: its squads, their members, single NPCs, baits
local swept, kept_ids = {}, {} -- squad or object id -> true: flagged offline / left alone
local sweep_count = {squads = 0, singles = 0, kept = 0}
local sweep_state = {next = 0, passes = 0, logged = 0}

local function own(id)
	if id then own_ids[id] = true end
	return id
end

-- A living stalker or monster that is neither the actor nor the probe's.
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

local function sweep_one(se, actor_pos)
	if foreign_creature(se) and on_this_level(se) and se.position:distance_to(actor_pos) <= SWEEP_RADIUS then
		return take_offline(se)
	end
	return 0
end

-- An online creature of db.storage: new flags, 1 when it is a foreign one still online within the radius, its distance.
local function sweep_online_one(id, st, actor_pos)
	local obj = st.object
	if not obj or id == 0 or own_ids[id] or not (IsStalker(obj) or IsMonster(obj)) or not obj:alive() then return 0, 0 end
	local d = obj:position():distance_to(actor_pos)
	local se = d <= SWEEP_RADIUS and alife():object(id)
	if not se or own_ids[se.group_id] then return 0, 0 end
	local new = take_offline(se)
	local group = se.group_id ~= 65535 and se.group_id or id
	return new, kept_ids[group] and 0 or 1, d
end

local function sweep(now)
	if now < sweep_state.next or not db.actor then return end
	sweep_state.next = now + SWEEP_PERIOD_MS
	local actor_pos = db.actor:position()
	local new, scope = 0, "online"
	if sweep_state.passes == 0 then
		-- Every ALife object once; by id when the engine's iterate_objects is missing or fails.
		scope = "all"
		local walked = pcall(function()
			alife():iterate_objects(function(se)
				local ok, n = pcall(sweep_one, se, actor_pos)
				if ok and n then new = new + n end
				return false
			end)
		end)
		if not walked then
			scope = "all_ids"
			for id = 1, 65534 do
				local se = alife():object(id)
				if se then
					local ok, n = pcall(sweep_one, se, actor_pos)
					if ok and n then new = new + n end
				end
			end
		end
	elseif sweep_state.passes % SWEEP_SQUADS_EVERY == 0 and SIMBOARD and SIMBOARD.squads then
		scope = "squads"
		for id, sq in pairs(SIMBOARD.squads) do
			if not (own_ids[id] or swept[id] or kept_ids[id]) then
				local ok, n = pcall(function() return sweep_one(alife():object(sq:commander_id()), actor_pos) end)
				if ok and n then new = new + n end
			end
		end
	end
	local online_n, nearest = 0
	for id, st in pairs(db.storage) do
		local ok, n, counted, d = pcall(sweep_online_one, id, st, actor_pos)
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

------------------------------------------------------------------------------------------------------------------------
-- Spawning (every id the probe creates is its own: the sweep leaves it alone)
------------------------------------------------------------------------------------------------------------------------
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
	own(sq.id)
	sq:create_npc(nil, pos, vid, gv)
	sq.__lock = true
	local ids = member_ids(sq)
	for _, id in ipairs(ids) do own(id) end
	while #ids > count do
		sq:remove_npc(ids[#ids])
		table.remove(ids)
	end
	local npc_section = squad_npc_section(section)
	while #ids < count and npc_section do
		own(sq:add_squad_member(npc_section, pos, vid, gv))
		local grown = member_ids(sq)
		if #grown <= #ids then break end
		ids = grown
		for _, id in ipairs(ids) do own(id) end
	end
	for _, id in ipairs(ids) do
		local se = alife():object(id)
		if se then SIMBOARD:setup_squad_and_group(se) end
	end
	return sq, ids
end

local function add_member(sq, section, vid)
	local pos, gv = level.vertex_position(vid), db.actor:game_vertex_id()
	local id = own(sq:add_squad_member(squad_npc_section(section), pos, vid, gv))
	local se = id and alife():object(id)
	if se then SIMBOARD:setup_squad_and_group(se) end
	return id
end

local function spawn_npc(section, vid)
	local se = alife():create(section, level.vertex_position(vid), vid, db.actor:game_vertex_id())
	return se and own(se.id)
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
	"da_action_search_overwatch"}

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

-- Logs every round a watched NPC fires (its magazine count going down) with its sight of its best enemy.
local ammo_seen = {}
local rounds_by = {} -- npc id -> rounds logged so far
local function poll_shots(npc, extra)
	local item = npc:active_item()
	local class = item and npc:ai_weapon_class(item) or ""
	if class == "" or class == "none" then return end
	local ammo = item:get_ammo_in_magazine()
	local last = ammo_seen[npc:id()]
	if last and last.id == item:id() and ammo < last.ammo then
		local enemy = npc:best_enemy()
		rounds_by[npc:id()] = (rounds_by[npc:id()] or 0) + last.ammo - ammo
		say("shot npc=%d role=%s comm=%s class=%s rounds=%d see=%d dist=%.0f%s", npc:id(), roles[npc:id()] or "-",
			npc:character_community(), class, last.ammo - ammo, enemy and npc:see(enemy) and 1 or 0,
			enemy and npc:position():distance_to(enemy:position()) or -1, extra or "")
	end
	ammo_seen[npc:id()] = {id = item:id(), ammo = ammo}
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
local phase_ok, phase_value = pcall(function() return qa_ai_phase end)
local PHASE = phase_ok and phase_value == "b" and "b" or "a"

local SAVE_NAME = "qa_ai_mid"
-- REPORT_AT leaves the novice bandits' latest give-up (~71 s, see the header) a margin; the save and the quit that
-- follow it end the run ~89 s after the first update (the harness's budget is 90 s after the load).
local WALL_AT, AWAY_AT, REPORT_AT, SAVE_AT = 8, 20, 78, 80
-- T's away spot: beyond the search's widest ring ([ai_search] spot_radius_max 60) plus what a searcher clears by sight
-- (clear_view 25) around the wall spot, farthest first; the ring points the search walks must not see it either.
local AWAY_RADII, AWAY_MIN = {120, 110, 130, 100, 140, 95}, 95
local SEARCH_RINGS = {30, 55}
-- The shooters spawn facing anywhere: T makes two noises (a reload each) so they turn to it, and T goes behind the
-- wall only once one of them has had him in sight for WALL_SEEN_MS, at WALL_LATEST at the latest; the wall phase
-- then lasts AWAY_AT - WALL_AT.
local CUE_AT, WALL_SEEN_MS, WALL_LATEST = {3, 5.5}, 2000, 16
local cues_done, wall_time = 0

local stage = "wait"
-- Where the probe put T, checked a second later: a teleport the engine undid would void the wall or the search.
local placed_check
local spots, vids = {}, {}
local shooters = {} -- bandits D1-D3 and mercs M1-M2
local target_id, target_hold, target_ready
local relations_forced = {}
local next_poll, next_pair, next_layout = 0, 0, 0
local last_seen, see_seen, searching_seen = {}, {}, {}
local action_seen = {} -- shooter id -> "brain/combat" action ids last logged
local first_seen = {} -- shooter id -> time_global of its first sight of T
local save_time

local function target() return online(target_id) end

local function save_exists(name)
	return getFS():exist("$game_saves$", name .. ".scop") ~= nil
end

-- W: 3-5 m to T's side, hidden from D by static geometry.
local function wall_spot(t_vid, t_deg, d)
	for _, side in ipairs({90, -90, 60, -60, 120, -120}) do
		for _, step in ipairs({3, 4, 5}) do
			local w_vid, w = vertex_at(t_vid, t_deg + side, step)
			if w_vid and not los(d, w) then return w_vid, w end
		end
	end
end

-- D2: 6-12 m beside D, seeing T and (strict) not W.
local function second_spot(d_vid, t_deg, t, w, strict)
	for _, beside in ipairs({90, -90, 60, -60, 120, -120}) do
		for _, gap in ipairs({6, 8, 10, 12}) do
			local d2_vid, d2 = vertex_at(d_vid, t_deg + beside, gap)
			if d2_vid and los(d2, t) and (not strict or not los(d2, w)) then return d2_vid, d2 end
		end
	end
end

-- A shooter spot D with a spot T 25 m in front in its sight, W beside T behind a wall and D2 beside D. Without a D2
-- hidden from W the second pass takes one that sees it: the mercs then keep their sight of T, only the merc half of
-- the wall check is lost.
local function find_wall()
	local actor_vid, actor = db.actor:level_vertex_id(), db.actor:position()
	local start = math.random(0, 11) * 30
	for _, strict in ipairs({true, false}) do
		for _, r in ipairs({50, 70, 90, 110}) do
			for k = 0, 11 do
				local d_vid, d = vertex_at(actor_vid, start + k * 30, r)
				for m = 0, d_vid and 17 or -1 do
					local t_deg = m * 20
					local t_vid, t = vertex_at(d_vid, t_deg, 25)
					local w_vid, w, d2_vid, d2
					if t_vid and los(d, t) and flat_dist(t, actor) <= 190 then w_vid, w = wall_spot(t_vid, t_deg, d) end
					if w_vid then d2_vid, d2 = second_spot(d_vid, t_deg, t, w, strict) end
					if d2_vid then
						if not strict then say("relaxed spot=D2 sees_wall=1") end
						return {D = d_vid, T = t_vid, W = w_vid, D2 = d2_vid}, {D = d, T = t, W = w, D2 = d2}
					end
				end
			end
		end
	end
end

local function layout()
	local found_vids, found = find_wall()
	if not found_vids then return "no_wall" end
	vids, spots = found_vids, found
	for name, p in pairs(spots) do
		say("spot name=%s x=%.1f y=%.1f z=%.1f from_actor=%.0f", name, p.x, p.y, p.z, flat_dist(p, db.actor:position()))
	end
	local sq_d, d_ids = spawn_squad("bandit_sim_squad_novice", vids.D, 3)
	if not sq_d then return "no_squad_D" end
	for i, id in ipairs(d_ids) do
		tag(id, "D" .. i)
		shooters[#shooters + 1] = id
	end
	local sq_m, m_ids = spawn_squad("merc_sim_squad_advanced", vids.D2, 2)
	if not sq_m then return "no_squad_D2" end
	for i, id in ipairs(m_ids) do
		tag(id, "M" .. i)
		shooters[#shooters + 1] = id
	end
	target_id = spawn_npc(first_section("sim_default_stalker_2", "sim_default_stalker_1"), vids.T)
	if not target_id then return "no_T" end
	tag(target_id, "T")
	target_hold = spots.T
end

-- T stands where it is put, invulnerable, under script control: it neither flees nor fights back. The hold is a walk
-- to where it already stands: a "stand" movement to its own spot never builds a path and asked for one every frame
-- (r4a8: 3664 "DetailPathManager has failed : from X to X"); a walk's path is built once and done.
local function hold_target(pos)
	local t = target()
	if not t then return false end
	t:invulnerable(true)
	if t:get_script() then t:script(false, "qa_ai") end
	t:script(true, "qa_ai")
	local act = entity_action()
	act:set_action(move(move.standing, move.walk, move.line, pos))
	act:set_action(cond(cond.time_end, 600000))
	t:command(act, false)
	target_hold = pos
	return true
end

-- T is everybody's sworn enemy; the bandits and the mercs leave each other alone.
local function force_relations()
	local t = target()
	if not t then return end
	for _, id in ipairs(shooters) do
		local npc = not relations_forced[id] and online(id)
		if npc then
			npc:force_set_goodwill(-1000, t)
			t:force_set_goodwill(-1000, npc)
			local all = true
			for _, other_id in ipairs(shooters) do
				local other = online(other_id)
				if not other then
					all = false
				elseif string.sub(roles[other_id], 1, 1) ~= string.sub(roles[id], 1, 1) then
					npc:force_set_goodwill(0, other)
				end
			end
			relations_forced[id] = all or nil
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

local function unseen_ms(id, now)
	return last_seen[id] and now - last_seen[id] or -1
end

local function poll(now)
	local t = target()
	for _, id in ipairs(shooters) do
		local npc = online(id)
		if npc and npc:alive() and t then
			local see = npc:see(t)
			if see then
				last_seen[id] = now
				first_seen[id] = first_seen[id] or now
			end
			if see_seen[id] ~= see then
				see_seen[id] = see
				say("see npc=%d role=%s comm=%s target=T value=%d d=%.0f", id, roles[id] or "-",
					npc:character_community(), see and 1 or 0, npc:position():distance_to(t:position()))
			end
			poll_belief(npc, t, "T")
			local gone = see and 0 or unseen_ms(id, now)
			poll_shots(npc, string.format(" target_see=%d unseen_ms=%d", see and 1 or 0, gone))
			-- The planner's choice, on change: which planner runs, and the combat sub-planner's action (B7 diagnosis).
			local manager = npc:motivation_action_manager()
			local brain = manager and manager:initialized() and manager:current_action_id() or -1
			local combat = combat_action(npc) or -1
			local key = brain .. "/" .. combat
			if action_seen[id] ~= key then
				action_seen[id] = key
				say("action npc=%d role=%s comm=%s brain=%d combat=%d", id, roles[id] or "-", npc:character_community(),
					brain, combat)
			end
			local searching = npc:ai_searching()
			if searching_seen[id] ~= searching then
				searching_seen[id] = searching
				say("searching npc=%d role=%s comm=%s value=%d", id, roles[id] or "-", npc:character_community(),
					searching and 1 or 0)
			end
		end
	end
end

-- How the merc pair keeps together while it searches.
local function log_pair()
	local a, b = online(role_ids.M1), online(role_ids.M2)
	if a and b and a:alive() and b:alive() then
		say("pair roles=M1,M2 d=%.1f searching=%d,%d", a:position():distance_to(b:position()),
			a:ai_searching() and 1 or 0, b:ai_searching() and 1 or 0)
	end
end

local function to_wall()
	local t = target()
	if not t then
		say("error T_offline")
		return
	end
	t:set_npc_position(spots.W)
	hold_target(spots.W)
	say("event wall x=%.1f z=%.1f from_T=%.1f", spots.W.x, spots.W.z, flat_dist(spots.W, spots.T))
	placed_check = {name = "wall", pos = spots.W, at = time_global() + 1000}
end

-- AWAY_MIN m and more from the wall, out of the sight of the shooters' spots, of where each of them stands now and of
-- the ground the search walks (the wall spot and rings around it): a searcher that found T would restart the search
-- and no give-up would come (r4a8: 60 m, nobody gave up by 58 s). Every sight line hidden first; else the spot hidden
-- from all shooters and the most of the search ground; else the most hidden one (logged).
local function move_away()
	local t = target()
	if not t then
		say("error T_offline")
		return
	end
	local blockers = {spots.D, spots.D2}
	for _, id in ipairs(shooters) do
		local npc = online(id)
		if npc and npc:alive() then blockers[#blockers + 1] = npc:position() end
	end
	local ground = {spots.W}
	for _, r in ipairs(SEARCH_RINGS) do
		for k = 0, 5 do
			local _, q = vertex_at(vids.W, k * 60 + r, r)
			if q then ground[#ground + 1] = q end
		end
	end
	local near_actor = function(q)
		return flat_dist(q, db.actor:position()) <= 190 and flat_dist(q, spots.W) >= AWAY_MIN
	end
	local shooters_blind = function(q) return near_actor(q) and hidden_from(q, blockers) end
	local vid, p, _, relaxed, score = find_spot_or_best(vids.W, AWAY_RADII, math.random(0, 35) * 10,
		function(q) return shooters_blind(q) and hidden_from(q, ground) end, shooters_blind,
		function(q) return hidden_count(q, ground) end)
	if relaxed then say("relaxed spot=away search_hidden=%d/%d", score or 0, #ground) end
	if not vid then
		vid, p, _, score = best_spot(vids.W, AWAY_RADII, math.random(0, 35) * 10, near_actor,
			function(q) return hidden_count(q, blockers) * 100 + hidden_count(q, ground) end)
		if vid then
			say("relaxed spot=away shooters_hidden=%d/%d search_hidden=%d/%d", math.floor(score / 100), #blockers,
				score % 100, #ground)
		end
	end
	if not vid then
		say("error no_away_spot")
		return
	end
	t:set_npc_position(p)
	hold_target(p)
	say("event away x=%.1f z=%.1f from_wall=%.0f", p.x, p.z, flat_dist(p, spots.W))
	placed_check = {name = "away", pos = p, at = time_global() + 1000}
end

local function print_beliefs()
	local t = target()
	for _, id in ipairs(shooters) do
		local npc = online(id)
		if npc and npc:alive() and t then dump_belief(npc, t, "T") end
	end
end

-- The roles ride in the save's script data, so the loaded run finds the same NPCs.
local function save_roles()
	local data = alife_storage_manager.get_state()
	data.qa_ai_roles = {}
	for id, role in pairs(roles) do data.qa_ai_roles[id] = role end
end

local function restore_roles()
	local data = alife_storage_manager.get_state()
	local saved = data and data.qa_ai_roles
	if not saved then return false end
	for id, role in pairs(saved) do
		roles[id], role_ids[role] = role, id
		if role == "T" then
			target_id = id
		else
			shooters[#shooters + 1] = id
		end
	end
	table.sort(shooters)
	return true
end

local function shooter_groups()
	local groups = {D = {}, M = {}}
	for _, id in ipairs(shooters) do
		local list = groups[string.sub(roles[id], 1, 1)]
		if list then list[#list + 1] = id end
	end
	return groups
end

------------------------------------------------------------------------------------------------------------------------
-- After the save: B1's night muzzle flash (R1 had no spot for it in r1k)
------------------------------------------------------------------------------------------------------------------------
-- NIGHT_WAIT s after the save command (the save's capture ran 0.93 s in r4a8; nothing may change the world while it
-- runs) time jumps to 01:00 as in ai_fair.lua, and an observer O and a shooter F are spawned FLASH_D m apart: two level
-- vertices with a clear static line between their eyes, both inside 190 m of the actor, 40 m clear of him and
-- FLASH_CLEAR m from every R4a group, the wall and T; the distance tolerance widens over FLASH_TOLS before the trial
-- gives up ("error no_flash_pair", then the run ends as it would have). O is held under script control looking at F's
-- muzzle; F is held, armed, stripped of its torch and neutral to every probe stalker both ways (not a friend of O: a
-- friend's flash makes no estimate; nobody fights it), and at FLASH_FIRE_AT fires a scripted burst at a ground point
-- away from O and the actor. Logged as in ai_fair.lua: "night", "spot name=O/F ... from_O=", F's "shot" and "fire*"
-- lines, O's "belief ... target=F src=..." lines (B1's flash, Parse-AiLog.py check_flash).
local NIGHT_WAIT, FLASH_READY_AT, FLASH_FIRE_AT, FLASH_DONE_AT = 2, 82.8, 83.3, 86.5
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

-- One NPC of the given character section in its own held squad (a lone spawn walks off to a smart terrain).
local function spawn_held(squad_section, npc_section, vid, role)
	local sq, list = spawn_squad(squad_section, vid, 1)
	if not sq then return nil end
	local pos, gv = level.vertex_position(vid), db.actor:game_vertex_id()
	local id = npc_section and own(sq:add_squad_member(npc_section, pos, vid, gv))
	if id then
		for _, old in ipairs(list) do sq:remove_npc(old) end
		local se = alife():object(id)
		if se then SIMBOARD:setup_squad_and_group(se) end
	else
		id = list[1]
	end
	if id then tag(id, role) end
	return id
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

-- The first pair of spots FLASH_D m (+-tolerance) apart with a flash line, O around the actor, F around O.
local function flash_pair()
	local actor_vid, actor = db.actor:level_vertex_id(), db.actor:position()
	local groups = {spots.D, spots.D2, spots.W}
	local t = target()
	if t then groups[#groups + 1] = t:position() end
	for _, id in ipairs(shooters) do
		local npc = online(id)
		if npc and npc:alive() then groups[#groups + 1] = npc:position() end
	end
	local fine = function(q)
		local a = flat_dist(q, actor)
		return a <= 190 and a >= 40 and far_from(q, groups, FLASH_CLEAR)
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
								return o_vid, o, f_vid, f, tol
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
	local o_vid, o, f_vid, f, tol = flash_pair()
	if not o_vid then
		say("error no_flash_pair tols=%d clear=%d", #FLASH_TOLS, FLASH_CLEAR)
		return false
	end
	local actor = db.actor:position()
	flash.o_pos, flash.f_pos, flash.state = o, f, "new"
	flash.aim = aim_away(f, o, actor, 15)
	flash.weapon = first_section("wpn_ak74")
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
		command_observer(o, vector():set(flash.f_pos.x, flash.f_pos.y + 1.4, flash.f_pos.z))
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

local function step_a(t, now)
	-- The level's own creatures go offline before anything is placed, and stay so (not while the save is captured).
	if stage ~= "saving" then sweep(now) end
	if stage == "wait" then
		if t < 1 then return end
		local failed = layout()
		if failed then
			finish("void " .. failed)
			return
		end
		stage = "aim"
	elseif stage == "saving" then
		if now >= save_time + NIGHT_WAIT * 1000 then
			say("saved name=%s exists=%d", SAVE_NAME, save_exists(SAVE_NAME) and 1 or 0)
			stage = flash_setup() and "night" or "night_done"
		end
		return
	elseif stage == "night" or stage == "night_done" then
		if stage == "night" then update_flash(t, now) end
		if t >= FLASH_DONE_AT or stage == "night_done" then
			flush_stats()
			finish(void_reason and ("void " .. void_reason) or "complete")
		end
		return
	end
	if not target_ready then target_ready = hold_target(target_hold) end
	force_relations()
	log_radios()
	if now >= next_poll then
		next_poll = now + 100
		poll(now)
	end
	if now >= next_layout then
		next_layout = now + 1000
		check_layout(shooter_groups(), {}, 200)
	end
	if stage == "aim" and cues_done < #CUE_AT and t >= CUE_AT[cues_done + 1] then
		cues_done = cues_done + 1
		local tt = target()
		if tt then
			level.ai_noise(tt:position(), 1.0, "reload", tt)
			say("event cue n=%d", cues_done)
		end
	end
	if placed_check and now >= placed_check.at then
		local tt = target()
		if tt then
			say("placed name=%s off=%.1f", placed_check.name, flat_dist(tt:position(), placed_check.pos))
		end
		placed_check = nil
	end
	local seen_long = false
	for _, when in pairs(first_seen) do
		seen_long = seen_long or now - when >= WALL_SEEN_MS
	end
	if stage == "aim" and t >= WALL_AT and (seen_long or t >= WALL_LATEST) then
		to_wall()
		wall_time = t
		stage = "wall"
	elseif stage == "wall" and t >= wall_time + AWAY_AT - WALL_AT then
		move_away()
		stage = "search"
	elseif stage == "search" then
		if now >= next_pair then
			next_pair = now + 2000
			log_pair()
		end
		if t >= REPORT_AT then
			print_beliefs()
			stage = "report"
		end
	elseif stage == "report" and t >= SAVE_AT then
		-- The beliefs once more in the frame of the save: a bandit that gave up since REPORT_AT forgot T, and R4b
		-- compares with what the save holds (the parser keeps each NPC's last "final" line).
		print_beliefs()
		save_roles()
		cmd("save " .. SAVE_NAME)
		save_time = now
		stage = "saving"
	end
end

local function step_b(t)
	if stage == "wait" and t >= 2 then
		if not restore_roles() then
			finish("void no_roles_in_save")
			return
		end
		log_radios()
		print_beliefs()
		flush_stats()
		stage = "printed"
	elseif stage == "printed" and t >= 4 then
		finish("complete")
	end
end

local function step()
	if PHASE == "b" then
		step_b(t_now())
	else
		step_a(t_now(), time_global())
	end
end

-- Hits on T, logged and emptied (the engine asks this global first; DA defines none). T's own invulnerability does
-- not hold: DA's binder resets it every 500 ms from its logic section (stalker_generic.update_invulnerability), and in
-- r4a5/r4a6 one hard hit killed T, so the "search" watched its corpse.
local function install_hit_hook()
	local previous = rawget(_G, "CAI_Stalker__BeforeHitCallback")
	_G.CAI_Stalker__BeforeHitCallback = function(npc, shit, bone_id)
		local is_target = npc and target_id and npc:id() == target_id
		if is_target and t0 and not finished then
			local who = shit.draftsman
			local id = who and who:id()
			local shooter = id and online(id)
			local see = shooter and shooter:see(npc)
			say("target_hit npc=%s role=%s comm=%s see=%d unseen_ms=%d power=%.2f", id and tostring(id) or "-",
				id and roles[id] or "-", shooter and IsStalker(shooter) and shooter:character_community() or "-",
				see and 1 or 0, see and 0 or (id and unseen_ms(id, time_global()) or -1), shit.power)
		end
		local result = true
		if previous then result = previous(npc, shit, bone_id) end
		if is_target then
			shit.power = 0
			shit.impulse = 0
		end
		return result
	end
end

-- T must stay alive for the wall and the search: its invulnerability is put back on every update, and a death is
-- logged once (the parser voids the search then).
local target_dead_logged
local function guard_target()
	local t = target_id and level.object_by_id(target_id)
	if not t then return end
	if t:alive() then
		if not t:invulnerable() then t:invulnerable(true) end
	elseif not target_dead_logged then
		target_dead_logged = true
		say("target_dead x=%.1f z=%.1f", t:position().x, t:position().z)
	end
end

local function on_first_update()
	cmd("g_pause_in_background 0")
	cmd("main_menu off")
	unpause()
	cmd("g_god 1")
	cmd("ai_ignore_actor 1")
	cmd("ai_log_period 5000")
	-- Per-NPC trace lines of combat drops and search handovers (engine side of B7).
	cmd("ai_trace 1")
	log_action_ids()
	t0 = time_global()
	say("start scenario=ai_save phase=%s level=%s save_time=%02d:%02d weather=%s", PHASE, level.name(),
		level.get_time_hours(), level.get_time_minutes(), tostring(level.get_weather()))
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
	if PHASE == "a" then guard_target() end
	local ok, err = pcall(step)
	if not ok then
		say("error lua=%s", tostring(err))
		finish("error")
	end
end

function on_game_start()
	subscribe_reports()
	if PHASE == "a" then install_hit_hook() end
	RegisterScriptCallback("actor_on_first_update", on_first_update)
	RegisterScriptCallback("actor_on_update", on_update)
end
