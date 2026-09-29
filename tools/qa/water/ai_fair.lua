-- R1 ai-fair (docs\dead-air\ai\PLAN.md 7.3): hive mind, voice / radio / HQ reach, hearing, night. The actor stands
-- still in god mode and never shoots; every NPC stays within 200 m of him (the ALife switch distance, C26).
--
--   tools\qa\Run-WaterQa.ps1 -Name ai-fair -Scenario ai_fair -Save fogqa -Seconds 150 `
--       -ExtraArgs "-force_flushlog -always_active" -Out "D:\Games\Dead Air\_work\ai-npc\qa"
--
-- Timeline (s after the first actor update): 0 the level's own creatures within 300 m go offline for the run (again
-- every 0.5 s for whatever walks in); 1 layout and spawn; 5 the actor's scripted hit on A1; 20 on C1; 35-60 steps and
-- a reload behind the calm loner E; then the scripted shooters 100 m from E fire by command, S1 an unsuppressed burst
-- (~61) and S2 a suppressed one (~64); 62 the night observer O, its crouched target T and the scripted shooter F 150 m
-- from O (down to 90 m where no clear line holds); 65 time jumps to 01:00; 75 T's torch on; 78 O turns to F; 80 F
-- fires (muzzle flash; O's view of it logged, the burst's flash counters printed on their own); 90 done. Lines
-- "[qa-ai] t=<s> <event> key=value ..." feed tools\qa\ai\Parse-AiLog.py.

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

-- The bearing (degrees, the convention of bearing()) from a to b.
local function bearing_to(a, b)
	return math.deg(math.atan2(b.x - a.x, b.z - a.z))
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

-- A clear static line between two spots, from eye height at a to eye height at b (to_height: another height at b).
local function los(a, b, to_height)
	local from = vector():set(a.x, a.y + 1.6, a.z)
	local dir = vector():set(b.x, b.y + (to_height or 1.6), b.z):sub(from)
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
local stage = "wait"
local next_poll, next_layout, next_b_bait = 0, 0, 0
local spots = {}
local ids = {A = {}, A4 = {}, B = {}, C = {}, C4 = {}, Cp = {}, E = {}, S1 = {}, S2 = {}, O = {}, T = {}, F = {}}
local armed = {}
local trials, trial_index, trial = {}, 0, nil
local awareness_seen = {}

-- The engine's own step and reload loudness on earth (sense lane): crouch 0.12, walk 0.35, run 0.7 x earth 0.9,
-- a reload 0.5 - heard in silence within loudness x 36 m (steps) and x 40 m (the rest).
local STEP_GAITS = {{"crouch", 0.108}, {"walk", 0.315}, {"run", 0.63}}
local STEP_DISTANCES = {30, 15, 8}
local RELOAD_LOUDNESS, RELOAD_DISTANCE = 0.5, 12
-- The step trials stop here even when a trial is still running: the bursts and the night keep their times.
local TRIALS_LATEST = 61.5

local function all_ids(names)
	local list = {}
	for _, name in ipairs(names) do
		for _, id in ipairs(ids[name]) do list[#list + 1] = id end
	end
	return list
end

local function mutant_section() return first_section("tushkano_weak", "rat_weak", "flesh_weak", "dog_weak") end

local function spawn_mutant(vid)
	local section = mutant_section()
	local se = section and alife():create(section, level.vertex_position(vid), vid, db.actor:game_vertex_id())
	return se and own(se.id)
end

-- One NPC of the given character section in its own held squad: a lone spawn walks off to a smart terrain, so the
-- listeners, shooters and the observer would not stay where the layout put them.
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
	if id then tag(id, role) ids[role][1] = id end
	return id
end

-- A spot whose distances are hard requirements and whose hidden sight lines are wanted: without a fully hidden spot the
-- best-hidden one is taken and logged, and the sight flips logged later tell what the NPCs actually saw.
local function place_quiet(center_vid, radii, start, must, hide_from)
	return find_spot_or_best(center_vid, radii, start,
		function(q) return (not must or must(q)) and hidden_from(q, hide_from) end, must,
		function(q) return hidden_count(q, hide_from) end)
end

local function place(name, center_vid, radii, start, must, hide_from)
	local vid, p, deg, relaxed, score = place_quiet(center_vid, radii, start, must, hide_from)
	if relaxed then say("relaxed spot=%s hidden=%d/%d", name, score or 0, #hide_from) end
	return vid, p, deg
end

-- place() over tiers of requirements, strictest first: a spot hidden from every sight line under any tier beats a
-- best-hidden one, which comes from the strictest tier that has a spot at all. A tier after the first is logged.
local function place_tiered(name, center_vid, radii, start, musts, hide_from)
	for i, must in ipairs(musts) do
		local vid, p, deg = find_spot(center_vid, radii, start,
			function(q) return must(q) and hidden_from(q, hide_from) end)
		if vid then
			if i > 1 then say("relaxed spot=%s tier=%d", name, i) end
			return vid, p, deg
		end
	end
	for i, must in ipairs(musts) do
		local vid, p, deg, score = best_spot(center_vid, radii, start, must,
			function(q) return hidden_count(q, hide_from) end)
		if vid then
			say("relaxed spot=%s tier=%d hidden=%d/%d", name, i, score or 0, #hide_from)
			return vid, p, deg
		end
	end
end

-- Requirement builders for place_tiered.
local function apart(points, d) return function(p) return far_from(p, points, d) end end
local function all_of(...)
	local tests = {...}
	return function(p)
		for _, test in ipairs(tests) do
			if not test(p) then return false end
		end
		return true
	end
end

-- A point d metres from `from` on the ground, pointing away from a and b (their bisector; sideways when the two lie on
-- opposite sides): where a scripted shooter aims, so no round flies toward the listener, the observer or the actor.
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

-- Every group's spot for one starting bearing into `spots`, or the first group that found none. The squads search
-- toward the actor after their hits (r1j: C walked 25 m in 20 s toward him and broke C-A at 41.8 s), so a margin above
-- each wanted separation is tried first; E wants quiet around it (r1j: army C' fought 99 m from E and E stopped being
-- calm) and the shooters want no hostile group in reach.
local function layout_spots(start)
	local actor_vid, actor = db.actor:level_vertex_id(), db.actor:position()
	local ring = {150, 160, 170, 180, 140, 190}

	local p1_vid, p1, deg = place("P1", actor_vid, {120, 125, 115, 130}, start, nil, {actor})
	if not p1_vid then return "no_P1" end
	spots.P1 = p1
	local a4_vid, a4 = place_tiered("A4", actor_vid, ring, deg + 180, {apart({p1}, 175), apart({p1}, 150)}, {actor})
	if not a4_vid then return "no_A4" end
	local b_vid, b = place_tiered("B", actor_vid, ring, deg + 90, {apart({p1, a4}, 175), apart({p1, a4}, 150)},
		{actor, p1, a4})
	if not b_vid then return "no_B" end
	local c_vid, c = place_tiered("C", actor_vid, ring, deg + 270, {apart({p1, a4, b}, 185), apart({p1, a4, b}, 150)},
		{actor, p1, a4, b})
	if not c_vid then return "no_C" end
	local c4_vid, c4 = place_tiered("C4", actor_vid, {190, 180, 170, 160, 150, 140, 120}, deg + 135,
		{apart({c}, 175), apart({c}, 150)}, {actor, p1, a4, b})
	if not c4_vid then return "no_C4" end
	-- C' also keeps clear of the bandits where the ring allows it: army and bandits fight on sight, and a fight next
	-- to B moves B's centre.
	local cp_vid, cp = place_tiered("Cp", actor_vid, {190, 180, 170, 160, 150, 140, 120}, deg + 45,
		{all_of(apart({c, c4}, 175), apart({p1, a4, b}, 120)), all_of(apart({c, c4}, 150), apart({p1, a4, b}, 100)),
			apart({c, c4}, 150)}, {actor, p1, a4, b})
	if not cp_vid then return "no_Cp" end
	local hostile = {p1, a4, c, c4, cp}
	local e_vid, e = place_tiered("E", actor_vid, {100, 90, 110, 80, 120, 70, 130, 60, 140, 150, 160}, deg + 200,
		{all_of(apart(hostile, 100), apart({b}, 150)), all_of(apart(hostile, 80), apart({b}, 120)),
			all_of(apart(hostile, 60), apart({b}, 100))}, {actor, p1, a4, b, c, c4, cp})
	if not e_vid then return "no_E" end
	-- The two shooters 100 m from E, out of its sight and of every hostile group, and off the actor's spot: A and C
	-- search for him after their hits (r1f: C walked into S at 35 m from the actor and fought S2).
	local near_actor = function(p) return flat_dist(p, actor) <= 195 and flat_dist(p, actor) >= 70 end
	local all_hostile = {p1, a4, b, c, c4, cp}
	local s_vid, s = place_tiered("S", e_vid, {100, 95, 105, 90, 110}, deg,
		{all_of(near_actor, apart(all_hostile, 100)), all_of(near_actor, apart(all_hostile, 80)),
			all_of(near_actor, apart(all_hostile, 60))}, {e, p1, a4, b, c, c4, cp})
	if not s_vid then return "no_S" end
	-- S2 stands 4-6 m beside S1, square to their line of fire (away from E and the actor), as far from E as S1.
	local aim = aim_away(s, e, actor, 15)
	local fire_deg = bearing_to(s, aim)
	local s2_vid, s2
	for _, side in ipairs({90, -90, 60, -60, 120, -120}) do
		for _, gap in ipairs({5, 4, 6}) do
			local v, q = vertex_at(s_vid, fire_deg + side, gap)
			if v and math.abs(flat_dist(q, e) - flat_dist(s, e)) <= 6 and not los(q, e) then s2_vid, s2 = v, q break end
		end
		if s2_vid then break end
	end
	if not s2_vid then
		say("relaxed spot=S2 same=1")
		s2_vid, s2 = s_vid, s
	end

	spots.A4, spots.B, spots.C, spots.C4, spots.Cp, spots.E, spots.S, spots.S2 = a4, b, c, c4, cp, e, s, s2
	spots.vids = {P1 = p1_vid, A4 = a4_vid, B = b_vid, C = c_vid, C4 = c4_vid, Cp = cp_vid, E = e_vid, S = s_vid,
		S2 = s2_vid}
end

-- The flash's line of sight by the engine's own rule: level.ai_line_vis casts the flash's full-test static ray (sense
-- los) and multiplies the vision transparency of what it crosses; the flash passes above [ai_awareness] opaque_below
-- (0.05). From O's eye (FLASH_EYE_H, standing) to F's muzzle (FLASH_MUZZLE_H), and FLASH_LINE_DH above and below, each
-- ray at least FLASH_VIS: a margin for where the eye and the muzzle really are. r4a9 placed its pair by one culled ray
-- at 1.6 m (O heard every round occluded: something stood between); r2n's strict check (any static hit rejects, both
-- ways) found no pair on Cordon.
local FLASH_EYE_H, FLASH_MUZZLE_H, FLASH_LINE_DH, FLASH_VIS = 1.7, 1.4, 0.2, 0.25
local function flash_line(o, f)
	for _, dh in ipairs({0, FLASH_LINE_DH, -FLASH_LINE_DH}) do
		local eye = vector():set(o.x, o.y + FLASH_EYE_H + dh, o.z)
		local muzzle = vector():set(f.x, f.y + FLASH_MUZZLE_H + dh, f.z)
		if level.ai_line_vis(eye, muzzle) < FLASH_VIS then return false end
	end
	return true
end

-- The muzzle range at a darkness (0 day .. 1 night) as the engine mixes [ai_light] muzzle_range (day, night).
local function muzzle_range(dark)
	local ini = system_ini()
	local text = ini:line_exist("ai_light", "muzzle_range") and ini:r_string_ex("ai_light", "muzzle_range") or ""
	local day, night = string.match(text, "([%d.]+)%s*,%s*([%d.]+)")
	day, night = tonumber(day) or 80, tonumber(night) or 300
	return day + (night - day) * dark
end

-- O's view of F's muzzle as the flash rule judges it, when F's burst is ordered and at each of its rounds (the 100 ms
-- poll): the head's direction against O's view cone, the line by the engine's rule (clear above 0.05), the distance
-- against the muzzle range at the current darkness, O's own sight of F (a flash from a shooter in sight is no news).
-- "flash_view at=<fire|shot> dot=<cos> angle=<deg> half_fov=<deg> in_view=<0/1> vis=<0..1> d=<m> range=<m>
-- dark=<0..1> see=<0/1>"; the muzzle is F's weapon, else FLASH_MUZZLE_H above F.
local function log_flash_view(at)
	local o, f = online(ids.O[1]), online(ids.F[1])
	if not (o and f and o:alive() and f:alive()) then return end
	local eye = o:bone_position("bip01_head")
	local item = f:active_item()
	local muzzle = item and vector():set(item:position())
	if not muzzle or muzzle:distance_to(f:position()) > 2.5 then
		muzzle = vector():set(f:position()):add(vector():set(0, FLASH_MUZZLE_H, 0))
	end
	local to = vector():set(muzzle):sub(eye)
	local d = to:magnitude()
	local dot = d > 0.01 and o:head_orientation():dotproduct(to:normalize()) or 1
	local angle = math.deg(math.acos(math.max(-1, math.min(1, dot))))
	local half = o:fov() * 0.5
	local dark = level.ai_darkness()
	say("flash_view at=%s dot=%.3f angle=%.1f half_fov=%.1f in_view=%d vis=%.2f d=%.0f range=%.0f dark=%.2f see=%d",
		at, dot, angle, half, angle <= half and 1 or 0, level.ai_line_vis(eye, muzzle), d, muzzle_range(dark), dark,
		o:see(f) and 1 or 0)
end

------------------------------------------------------------------------------------------------------------------------
-- The scripted shooters: S1 and S2 (the bursts E hears, P6) and F (the flash O sees, B1)
------------------------------------------------------------------------------------------------------------------------
-- Each one is under script control from the moment it comes online, so it neither fights nor wanders (r1j: S1 and S2
-- were drawn into the level's fight and never fired at their bait; r1g, r1j: F never engaged its bait at night), and
-- fires by command: hold (weapon away) -> ready (weapon out, aimed at a point on the ground away from the listeners)
-- -> fire -> idle once BURST rounds are logged. A command that fired no round within FIRE_WINDOW is given once more.
-- Lines "gunner role=<r> ...", "fire role=<r> try=<n> weapon=<section> ammo=<rounds in the magazine>", "fire_end
-- role=<r> rounds=<n> tries=<n>" and "fire_missed role=<r> why=<...>"; the rounds are the "shot" lines of poll_shots.
local BURST, FIRE_WINDOW, FIRE_TRIES = 3, 2.5, 2
local GUNNER_WEAPONS = {S1 = {"wpn_ak74"}, S2 = {"wpn_vintorez", "wpn_val"}, F = {"wpn_ak74"}}
local gunners = {} -- role -> {aim, weapon, state, ready_at, fire_at, stop_at, tries, rounds0, rounds_first}

local function add_gunner(role, aim)
	gunners[role] = {aim = aim, weapon = first_section(unpack(GUNNER_WEAPONS[role])), state = "new", tries = 0}
end

-- The counters of F's burst on their own: the running period is printed and reset when the burst is ordered and again
-- FLASH_WINDOW_TAIL s after F is done (a flash waits at most stimulus_ttl_ms 1.5 s in an inbox), so the periods printed
-- after "flash_window open" up to "flash_window close" hold the burst's s_flash, s_flash_q and s_flash_x_*
-- (Parse-AiLog.py). The probe's other stalkers within the muzzle range of F are logged: they are observers too.
local FLASH_WINDOW_TAIL = 1.6
local flash_window -- nil, "open", "closed"
local function open_flash_window()
	if flash_window then return end
	flash_window = "open"
	local f, near = online(ids.F[1]), {}
	if f then
		local range = muzzle_range(level.ai_darkness())
		for id, role in pairs(roles) do
			local npc = role ~= "O" and role ~= "F" and online(id)
			local d = npc and npc:position():distance_to(f:position())
			if npc and npc:alive() and IsStalker(npc) and d <= range then
				near[#near + 1] = string.format("%s:%.0f", role, d)
			end
		end
	end
	table.sort(near)
	say("flash_window open others=%d near=%s", #near, #near > 0 and table.concat(near, ",") or "-")
	flush_stats()
	log_flash_view("fire")
end

local function close_flash_window(t)
	local g = gunners.F
	if flash_window ~= "open" or not g then return end
	if g.state == "done" and not g.done_at then g.done_at = t end
	if (g.done_at and t >= g.done_at + FLASH_WINDOW_TAIL) or t >= 89 then
		flash_window = "closed"
		say("flash_window close")
		flush_stats()
	end
end

-- The weapon arm_only gave, else the NPC's best firearm (arm_only left it no other one); never a knife.
local function gunner_weapon(npc, g)
	local weapon = g.weapon and npc:object(g.weapon)
	if weapon then return weapon end
	local best = npc:best_weapon()
	local class = best and npc:ai_weapon_class(best) or ""
	if class ~= "" and class ~= "none" then return best end
end

-- The gunners are the trials' instruments, not their subjects: neutral to every probe NPC both ways (no squad fights
-- one it sees or hears, and none becomes the gunner's selected enemy: DA's fire gate lets no round leave while a
-- selected enemy is out of sight, and a scripted burst at a point has none in sight), and an enemy it remembers anyway
-- (a mutant) is forgotten before each weapon command. The count of probe stalkers made neutral.
local function neutralize(npc)
	local n = 0
	for id in pairs(roles) do
		local other = id ~= npc:id() and online(id)
		if other and other:alive() and IsStalker(other) then
			npc:force_set_goodwill(0, other)
			other:force_set_goodwill(0, npc)
			n = n + 1
		end
	end
	local enemy = npc:best_enemy()
	if enemy then npc:enable_memory_object(enemy, false) end
	return n
end

-- A fresh command in place of whatever the NPC was doing (a queued command would wait for the previous one); false
-- when a weapon command finds no weapon (the NPC keeps its last command then). A hold walks to where the NPC stands: a
-- "stand" movement to its own spot never builds a path and asked for one every frame (r1i: 1546 "DetailPathManager has
-- failed : from X to X" each for O and T); its danger mental state (the script movement applies it) stays for the
-- weapon commands. These carry no movement: a script movement resets the item goal every frame until it is done,
-- which would put the weapon away. "ready" is the object action activate (the item out and idle): the idle action with
-- no weapon in hand completes at once and never takes it out.
local function command_gunner(npc, g, action)
	local weapon = action ~= "hold" and gunner_weapon(npc, g)
	if action ~= "hold" and not weapon then return false end
	g.neutral = neutralize(npc)
	if npc:get_script() then npc:script(false, "qa_ai") end
	npc:script(true, "qa_ai")
	local act = entity_action()
	if action == "hold" then
		act:set_action(anim(anim.danger))
		act:set_action(move(move.standing, move.walk, move.line, npc:position()))
		act:set_action(look(look.point, vector():set(g.aim.x, g.aim.y + 1.3, g.aim.z)))
	else
		npc:set_mental_state(anim.danger)
		act:set_action(anim(anim.danger))
		act:set_action(look(look.fire_point, vector():set(g.aim.x, g.aim.y, g.aim.z)))
		local verb = action == "fire" and object.fire1 or action == "ready" and object.activate or object.idle
		act:set_action(object(weapon, verb, BURST))
	end
	act:set_action(cond(cond.time_end, 600000))
	npc:command(act, false)
	return true
end

local function gunner_fire(role, npc, g, t)
	if not command_gunner(npc, g, "fire") then
		say("fire_missed role=%s why=no_weapon", role)
		g.state = "done"
		return
	end
	g.tries = g.tries + 1
	g.rounds0 = rounds_by[npc:id()] or 0
	g.rounds_first = g.rounds_first or g.rounds0
	g.stop_at = t + FIRE_WINDOW
	g.state = "fire"
	local weapon = gunner_weapon(npc, g)
	say("fire role=%s try=%d weapon=%s ammo=%d aim_d=%.0f", role, g.tries, weapon:section(),
		weapon:get_ammo_in_magazine(), flat_dist(npc:position(), g.aim))
end

-- A weak mutant 8-12 m from the NPC in its sight (the fallback of a burst that never left).
local function bait_near(npc)
	local p = npc:position()
	local vid = find_spot(npc:level_vertex_id(), {10, 8, 12}, math.random(0, 35) * 10, function(q) return los(p, q) end)
	return vid and spawn_mutant(vid)
end

local function update_gunner(role, t)
	local g = gunners[role]
	if not g or g.state == "done" then return end
	local npc = online(ids[role][1])
	if not (npc and npc:alive()) then
		if g.fire_at and t >= g.fire_at then
			say("fire_missed role=%s why=%s", role, npc and "dead" or "offline")
			g.state = "done"
		end
		return
	end
	if g.state == "new" then
		if g.weapon and not armed[role] then
			arm_only(npc, g.weapon)
			armed[role] = g.weapon
			say("armed role=%s weapon=%s", role, g.weapon)
		end
		command_gunner(npc, g, "hold")
		g.state = "hold"
		say("gunner role=%s x=%.1f z=%.1f aim_d=%.0f neutral=%d", role, npc:position().x, npc:position().z,
			flat_dist(npc:position(), g.aim), g.neutral or 0)
	elseif g.state == "hold" and g.ready_at and t >= g.ready_at then
		-- The weapon of arm_only may still be on its way into the inventory: asked again next update.
		if command_gunner(npc, g, "ready") then
			g.state = "ready"
		elseif t >= g.fire_at + FIRE_WINDOW then
			say("fire_missed role=%s why=no_weapon", role)
			g.state = "done"
		end
	elseif g.state == "ready" and t >= g.fire_at then
		if role == "F" then open_flash_window() end
		gunner_fire(role, npc, g, t)
	elseif g.state == "fire" then
		local rounds = (rounds_by[npc:id()] or 0) - g.rounds0
		if rounds == 0 and t >= g.stop_at and g.tries < FIRE_TRIES then
			gunner_fire(role, npc, g, t)
		elseif rounds == 0 and t >= g.stop_at then
			-- No scripted round left at all: the gunner's own fight with a bait beside it is the last resort (the way
			-- before the sweep); its rounds are logged as ever.
			npc:script(false, "qa_ai")
			g.state = "done"
			say("fire_missed role=%s why=no_rounds tries=%d bait=%d", role, g.tries, bait_near(npc) and 1 or 0)
		elseif rounds >= BURST or t >= g.stop_at then
			command_gunner(npc, g, "idle")
			g.state = "done"
			say("fire_end role=%s rounds=%d tries=%d", role, (rounds_by[npc:id()] or 0) - g.rounds_first, g.tries)
		end
	end
end

local function layout()
	-- A bearing whose ring cannot hold every group says nothing about the game: the next bearings are tried (30 degree
	-- steps) before the run is void.
	local base, failed = math.random(0, 35) * 10
	for i = 0, 11 do
		failed = layout_spots((base + i * 30) % 360)
		if not failed then break end
		say("layout_retry start=%d failed=%s", (base + i * 30) % 360, failed)
	end
	if failed then return failed end
	local v, actor = spots.vids, db.actor:position()
	for name in pairs(v) do
		local p = spots[name]
		say("spot name=%s x=%.1f y=%.1f z=%.1f from_actor=%.0f", name, p.x, p.y, p.z, flat_dist(p, actor))
	end

	local sq_a, a_ids = spawn_squad("bandit_sim_squad_novice", v.P1, 3)
	if not sq_a then return "no_squad_A" end
	for i, id in ipairs(a_ids) do tag(id, "A" .. i) ids.A[#ids.A + 1] = id end
	local a4_id = add_member(sq_a, "bandit_sim_squad_novice", v.A4)
	if a4_id then tag(a4_id, "A4") ids.A4[1] = a4_id end

	local _, b_ids = spawn_squad("bandit_sim_squad_novice", v.B, 4)
	for i, id in ipairs(b_ids) do tag(id, "B" .. i) ids.B[#ids.B + 1] = id end
	-- Two weak mutants beside B keep it fighting from the start (the stock pooling only reaches combat members).
	for _ = 1, 2 do
		local m_vid = find_spot(v.B, {15, 10, 20}, math.random(0, 35) * 10)
		if m_vid then spawn_mutant(m_vid) end
	end

	local sq_c, c_ids = spawn_squad("army_sim_squad_advanced", v.C, 3)
	if not sq_c then return "no_squad_C" end
	for i, id in ipairs(c_ids) do tag(id, "C" .. i) ids.C[#ids.C + 1] = id end
	local c4_id = add_member(sq_c, "army_sim_squad_advanced", v.C4)
	if c4_id then tag(c4_id, "C4") ids.C4[1] = c4_id end
	local _, cp_ids = spawn_squad("army_sim_squad_advanced", v.Cp, 3)
	for i, id in ipairs(cp_ids) do tag(id, "Cp" .. i) ids.Cp[#ids.Cp + 1] = id end

	local loner = first_section("sim_default_stalker_2", "sim_default_stalker_1")
	spawn_held("stalker_sim_squad_advanced", loner, v.E, "E")
	spawn_held("stalker_sim_squad_advanced", loner, v.S, "S1")
	spawn_held("stalker_sim_squad_advanced", loner, v.S2, "S2")
	add_gunner("S1", aim_away(spots.S, spots.E, actor, 15))
	add_gunner("S2", aim_away(spots.S2, spots.E, actor, 15))
end

local function setup_trials()
	for _, gait in ipairs(STEP_GAITS) do
		for _, d in ipairs(STEP_DISTANCES) do
			trials[#trials + 1] = {kind = "step", gait = gait[1], loudness = gait[2], d = d}
		end
	end
	trials[#trials + 1] = {kind = "reload", gait = "reload", loudness = RELOAD_LOUDNESS, d = RELOAD_DISTANCE}
end

-- Each trial: 5 footsteps (or one reload) behind E at the given distance over 2 s, then E's state 2.5 s after start.
local function run_trials(now)
	local e = online(ids.E[1])
	if not e then return end
	if not trial then
		trial_index = trial_index + 1
		trial = trials[trial_index]
		if not trial then return true end
		local dir = e:direction()
		local p = e:position()
		local pos = vector():set(p.x - dir.x * trial.d, p.y, p.z - dir.z * trial.d)
		-- A step sounds where a foot lands, not at E's height: on a slope that point is in or above the ground.
		local vid = level.vertex_id(vector():set(pos.x, pos.y + 2, pos.z))
		if vid and vid < 4294967295 and level.valid_vertex_id(vid) then
			local ground = level.vertex_position(vid)
			if flat_dist(ground, pos) <= 1.5 then pos = ground end
		end
		trial.pos, trial.dy = pos, pos.y - p.y
		trial.start, trial.emitted = time_global(), 0
		-- Calm = not fighting; a danger from far gunfire (the level's own fights) is logged but does not count. What E
		-- fights when it is not calm is logged with it.
		local enemy = e:best_enemy()
		trial.calm = enemy == nil
		trial.enemy = enemy and string.format(" enemy=%s enemy_d=%.0f", name_of(enemy),
			e:position():distance_to(enemy:position())) or ""
		trial.alert = e:best_danger() ~= nil
	end
	local elapsed = time_global() - trial.start
	local count = trial.kind == "step" and 5 or 1
	if trial.emitted < count and elapsed >= trial.emitted * 400 then
		level.ai_noise(trial.pos, trial.loudness, trial.kind, nil)
		trial.emitted = trial.emitted + 1
	end
	if elapsed >= 2500 then
		-- Suspicious = a fresh enemy-sound danger near the noise (the suspicion of a heard estimate, C10), looked up
		-- among all of E's dangers: the distant gunfire of the level's own fights ranks above it as the best danger.
		local danger = e:best_danger()
		local fresh = danger and danger:time() >= trial.start
		local p = fresh and danger:position()
		local near_type = e:ai_danger_near(trial.pos, 10 + 0.5 * trial.d, trial.start)
		local near = near_type == danger_object.enemy_sound
		say("hearing gait=%s d=%d calm=%d alert=%d suspicious=%d near_type=%d danger_type=%s err=%.1f dy=%.1f%s",
			trial.gait, trial.d, trial.calm and 1 or 0, trial.alert and 1 or 0, near and 1 or 0, near_type,
			fresh and tostring(danger:type()) or "-", p and p:distance_to(trial.pos) or -1, trial.dy, trial.enemy)
		trial = nil
	end
end

-- F: d m from O (FLASH_DS, nearest first to 150 m, B1's trial) with a flash line at any bearing (O turns to face F
-- before the burst, so no view cone limits the spot). The shorter distances stay beyond the day's muzzle range, so the
-- flash still needs the night, and every one within 0.9 x the range at the trial's darkness (FLASH_DARK). Inside 190 m
-- of the actor and 40 m clear of him; the spot far from and hidden from the hostile groups wins. {vid, position, d}.
local FLASH_DS = {150, 140, 160, 130, 120, 110, 100, 90}
local FLASH_DARK = 1 -- the clock's darkness at 01:00, where the time jump lands ([ai_light] night_hours)
local function flash_spot(o_vid, o, d)
	if d > muzzle_range(FLASH_DARK) * 0.9 then return nil end
	local actor = db.actor:position()
	local hostile = {spots.P1, spots.A4, spots.B, spots.C, spots.C4, spots.Cp}
	local best, best_score
	for k = 0, 71 do
		local v, q = vertex_at(o_vid, k * 5, d)
		local a = q and flat_dist(q, actor)
		if v and a <= 190 and a >= 40 and flash_line(o, q) then
			local score = (far_from(q, hostile, 80) and 100 or 0) + hidden_count(q, hostile) * 10
				- math.abs(flat_dist(q, o) - d)
			if not best_score or score > best_score then best, best_score = {v, q, d}, score end
		end
	end
	return best
end

-- O's candidates in place()'s order: per tier of requirements the spots hidden from every hostile group first, then
-- the rest by the number of groups they hide from. {vid, p, tier, hidden}.
local function observer_candidates(actor_vid, radii, start, tiers, hostile)
	local list, taken = {}, {}
	for tier, must in ipairs(tiers) do
		local hidden, rest = {}, {}
		for _, d in ipairs(radii) do
			for k = 0, 35 do
				local vid, p = vertex_at(actor_vid, (start + k * 10) % 360, d)
				if vid and not taken[vid] and must(p) then
					taken[vid] = true
					local item = {vid = vid, p = p, tier = tier, hidden = hidden_count(p, hostile), order = #rest}
					if item.hidden == #hostile then hidden[#hidden + 1] = item else rest[#rest + 1] = item end
				end
			end
		end
		table.sort(rest, function(a, b) return a.hidden > b.hidden or (a.hidden == b.hidden and a.order < b.order) end)
		for _, item in ipairs(hidden) do list[#list + 1] = item end
		for _, item in ipairs(rest) do list[#list + 1] = item end
	end
	return list
end

-- T: 40 m from O, crouched in the open, hidden from the other groups' sight where the ground allows. The spot and the
-- notes of its relaxations (its "relaxed spot=T ..." lines).
local function target_spot(o_vid, o, groups)
	local hide_t = {spots.C, spots.C4, spots.Cp, spots.E, spots.S}
	local line = function(p) return los(o, p) end
	-- Nothing static on the line to T's head (a bush there slows O's meter down).
	local low = function(p) return los(o, p, 1.0) end
	local clear = function(p) return far_from(p, groups, 60) end
	local tiers = {
		{all_of(line, low, clear)},
		{all_of(line, low), "groups_near=1"},
		{line, "low_line=0"},
	}
	local start = math.random(0, 35) * 10
	for _, tier in ipairs(tiers) do
		local vid, p, _, relaxed, score = place_quiet(o_vid, {40, 38, 42}, start, tier[1], hide_t)
		if vid then
			local notes = {}
			if tier[2] then notes[#notes + 1] = tier[2] end
			if relaxed then notes[#notes + 1] = string.format("hidden=%d/%d", score or 0, #hide_t) end
			return vid, p, notes
		end
	end
end

-- O, F and T together: over O's first FLASH_O_TRIES candidates the pair nearest 150 m, then the first candidate with
-- a T spot too (r1k, r2n: the O placed first had no F spot and the flash trial was lost; r1l: the first candidate's
-- nearest was 120 m). With no F spot anywhere the first O with a T spot, and the night trial runs without the flash
-- ("error no_flash_spot"). Every group counts, not only the events: a squad that lost its contact walks to it, and a
-- fight or a torch next to O or T decides the night trial (r1f: C' 19 m from T lit it and killed O); O's second tier
-- keeps clear of the events alone.
local FLASH_O_TRIES = 12
local function night_layout()
	local actor_vid, actor = db.actor:level_vertex_id(), db.actor:position()
	local events = {spots.P1, spots.C, spots.E, spots.S}
	local hostile = {spots.P1, spots.A4, spots.B, spots.C, spots.C4, spots.Cp}
	local groups = {spots.P1, spots.A4, spots.B, spots.C, spots.C4, spots.Cp, spots.E, spots.S}
	local candidates = observer_candidates(actor_vid, {150, 160, 170, 180, 140, 190, 130, 120},
		math.random(0, 35) * 10, {
			function(p) return far_from(p, events, 100) and far_from(p, groups, 80) end,
			function(p) return far_from(p, events, 100) end,
		}, hostile)
	local tries = math.min(#candidates, FLASH_O_TRIES)
	local pick
	for _, d in ipairs(FLASH_DS) do
		for i = 1, tries do
			local c = candidates[i]
			local flash = flash_spot(c.vid, c.p, d)
			local t_vid, t, notes
			if flash then t_vid, t, notes = target_spot(c.vid, c.p, groups) end
			if t_vid then
				pick = {o = c, t_vid = t_vid, t = t, notes = notes, flash = flash, tried = i}
				break
			end
		end
		if pick then break end
	end
	for i = 1, pick and 0 or tries do
		local c = candidates[i]
		local t_vid, t, notes = target_spot(c.vid, c.p, groups)
		if t_vid then
			pick = {o = c, t_vid = t_vid, t = t, notes = notes, tried = i}
			break
		end
	end
	if not pick then return #candidates == 0 and "no_O" or "no_T" end

	local o_vid, o = pick.o.vid, pick.o.p
	if pick.o.tier > 1 then say("relaxed spot=O groups_near=1") end
	if pick.o.hidden < #hostile then say("relaxed spot=O hidden=%d/%d", pick.o.hidden, #hostile) end
	for _, note in ipairs(pick.notes) do say("relaxed spot=T %s", note) end
	local t_vid, t = pick.t_vid, pick.t
	spots.O, spots.T, spots.vids.O, spots.vids.T = o, t, o_vid, t_vid
	say("spot name=O x=%.1f y=%.1f z=%.1f from_actor=%.0f tried=%d of=%d", o.x, o.y, o.z, flat_dist(o, actor),
		pick.tried, tries)
	say("spot name=T x=%.1f y=%.1f z=%.1f from_O=%.0f", t.x, t.y, t.z, flat_dist(t, o))
	spawn_held("stalker_sim_squad_veteran", first_section("sim_default_stalker_3", "sim_default_stalker_2"),
		o_vid, "O")
	local t_id = spawn_npc(first_section("sim_default_bandit_2", "sim_default_bandit_1"), t_vid)
	if t_id then tag(t_id, "T") ids.T[1] = t_id end

	-- F now, so it is online, held, armed and stripped of its torch long before its burst.
	local flash = pick.flash
	if not flash then
		say("error no_flash_spot tried=%d", tries)
		return
	end
	local f_vid, f = flash[1], flash[2]
	spawn_held("stalker_sim_squad_advanced", first_section("sim_default_stalker_2", "sim_default_stalker_1"),
		f_vid, "F")
	spots.F, spots.vids.F = f, f_vid
	if flash[3] ~= FLASH_DS[1] then say("relaxed spot=F want=%d", flash[3]) end
	say("spot name=F x=%.1f y=%.1f z=%.1f from_O=%.0f want=%d from_actor=%.0f", f.x, f.y, f.z, flat_dist(f, o),
		flash[3], flat_dist(f, actor))
	add_gunner("F", aim_away(f, o, actor, 15))
	gunners.F.ready_at, gunners.F.fire_at = 78, 80
end

-- Releases every torch the NPC carries; the count of those released.
local function strip_torches(npc)
	local torches = {}
	npc:iterate_inventory(function(_, item)
		if item:section() == "device_torch" then torches[#torches + 1] = item:id() end
	end, npc)
	local released = 0
	for _, id in ipairs(torches) do
		local se = alife():object(id)
		if se then
			alife():release(se, true)
			released = released + 1
		end
	end
	return released
end

-- T: invulnerable, no torch, crouched and still under script control until its torch is switched on. It faces O, so
-- its torch shines at O: the frontal case of B1 (r1i: T faced away from O on its spawn heading, its lamp glowed side-on
-- and O took 6.2 s to identify it; r1h faced O by chance, 0.67 s). Its hold is a walk to its own spot (see
-- command_gunner).
local target_ready
local function prepare_target()
	local target = online(ids.T[1])
	if not target or target_ready then return end
	target_ready = true
	target:invulnerable(true)
	strip_torches(target)
	target:script(true, "qa_ai")
	-- A crouch needs a mental state other than free (stalker_movement_manager_base asserts it), and a scripted
	-- action sets the body before the mind: the mind goes first on its own.
	target:set_mental_state(anim.danger)
	local act = entity_action()
	act:set_action(anim(anim.danger))
	act:set_action(move(move.crouch, move.walk, move.line, target:position()))
	if spots.O then act:set_action(look(look.point, vector():set(spots.O.x, spots.O.y + 1.6, spots.O.z))) end
	act:set_action(cond(cond.time_end, 120000))
	target:command(act, false)
	say("target_ready id=%d", target:id())
end

-- O stands where it was put, a calm sentry. Until the night falls it faces away from T, so its meter starts from
-- zero in the dark; then it looks at T's spot, so the test is the frontal case.
local function command_observer(o, point)
	if o:get_script() then o:script(false, "qa_ai") end
	o:script(true, "qa_ai")
	local act = entity_action()
	act:set_action(anim(anim.free))
	act:set_action(move(move.standing, move.walk, move.line, o:position()))
	act:set_action(look(look.point, point))
	act:set_action(cond(cond.time_end, 120000))
	o:command(act, false)
end

local observer_ready
local function prepare_observer()
	local o, target = online(ids.O[1]), online(ids.T[1])
	if not (o and target) or observer_ready then return end
	observer_ready = true
	local away = vector():set(o:position()):mul(2):sub(target:position())
	command_observer(o, away)
	say("observer_ready id=%d facing=away", o:id())
end

local function observer_face_target()
	local o, target = online(ids.O[1]), online(ids.T[1])
	if not (o and target) then return end
	command_observer(o, target:position())
	say("observer_facing target=T")
end

-- After the torch trial O turns to F's spot, so the flash is in its view cone (at eye height: F's muzzle).
local function observer_face_flash()
	local o = online(ids.O[1])
	if not (o and o:alive() and spots.F) then return end
	command_observer(o, vector():set(spots.F.x, spots.F.y + 1.4, spots.F.z))
	say("observer_facing target=F")
end

local function jump_to_night()
	local h, m = level.get_time_hours(), level.get_time_minutes()
	local minutes = ((25 * 60) - (h * 60 + m)) % (24 * 60)
	level.change_game_time(0, math.floor(minutes / 60), minutes % 60)
	level.set_weather(level.get_weather(), true)
	say("night from=%02d:%02d added_min=%d", h, m, minutes)
end

local torch_given, torch_lit
local function torch_on()
	local target = online(ids.T[1])
	if not target then return end
	if not torch_given then
		torch_given = true
		give(target:id(), "device_torch", 1)
		return
	end
	local torch = target:object("device_torch")
	if torch and not torch_lit then
		-- DA's night torch rule may have lit it already; the parser needs the moment either way.
		if not torch:attachable_item_enabled() then torch:enable_attachable_item(true) end
		torch_lit = true
		-- How squarely T's heading points at O (1 = straight at O): the lamp's glow toward O follows it.
		local o, facing = online(ids.O[1]), -2
		if o then
			local to_o = vector():set(o:position()):sub(target:position())
			to_o.y = 0
			local heading = vector():set(target:direction())
			heading.y = 0
			if to_o:magnitude() > 0.01 and heading:magnitude() > 0.01 then
				facing = to_o:normalize():dotproduct(heading:normalize())
			end
		end
		say("torch_on id=%d facing=%.2f", target:id(), facing)
	end
end

local function poll_awareness(observer, target, name)
	local value = observer:ai_awareness(target)
	local key = observer:id() .. ":" .. target:id()
	local last = awareness_seen[key]
	if not last or math.abs(value - last) >= 0.1 or (value >= 1) ~= (last >= 1) then
		awareness_seen[key] = value
		say("awareness npc=%d role=%s target=%s value=%.2f see=%d", observer:id(), roles[observer:id()] or "-", name,
			value, observer:see(target) and 1 or 0)
	end
end

local WATCH = {"A", "A4", "B", "C", "C4", "Cp", "E"}
local observer_dead

local function poll(now)
	local actor = db.actor
	for _, id in ipairs(all_ids(WATCH)) do
		local npc = online(id)
		if npc and npc:alive() then
			poll_belief(npc, actor, "actor")
			poll_shots(npc)
		end
	end
	local e = online(ids.E[1])
	for _, name in ipairs({"S1", "S2"}) do
		local s = online(ids[name][1])
		if e and s and e:alive() then poll_belief(e, s, name) end
	end
	local o, target, f = online(ids.O[1]), online(ids.T[1]), online(ids.F[1])
	-- A dead observer is logged once and no longer polled (see() of a dead NPC is a script error).
	if o and not o:alive() and not observer_dead then
		observer_dead = true
		say("observer_dead npc=%d", o:id())
	end
	if o and target and o:alive() then
		poll_belief(o, target, "T")
		poll_awareness(o, target, "T")
	end
	-- O's meter on F too: the flash's cue can identify F in the update that processed the flash, and sight then
	-- replaces the flash's estimate before any poll reads it (r1l).
	if o and f and o:alive() then
		poll_belief(o, f, "F")
		poll_awareness(o, f, "F")
	end
	for _, name in ipairs({"S1", "S2", "F"}) do
		local shooter = online(ids[name][1])
		if shooter and shooter:alive() then
			local before = rounds_by[shooter:id()] or 0
			poll_shots(shooter)
			if name == "F" and (rounds_by[shooter:id()] or 0) > before then log_flash_view("shot") end
		end
	end
end

local radios_logged = {}
local function log_radios()
	for id, role in pairs(roles) do
		local npc = not radios_logged[id] and online(id)
		if npc then
			radios_logged[id] = true
			local se = alife():object(id)
			say("npc id=%d role=%s comm=%s rank=%d radio=%d leader=%d group=%s", id, role, npc:character_community(),
				npc:character_rank(), npc:ai_has_radio() and 1 or 0, npc:ai_is_leader() and 1 or 0,
				se and string.format("%s/%s/%s", tostring(se.team), tostring(se.squad), tostring(se.group)) or "-")
		end
	end
end

local function final()
	local actor = db.actor
	for _, id in ipairs(all_ids({"A", "A4", "B", "C", "C4", "Cp", "E", "O"})) do
		local npc = online(id)
		if npc then dump_belief(npc, actor, "actor") end
	end
	flush_stats()
	finish(void_reason and ("void " .. void_reason) or "complete")
end

local LAYOUT_PAIRS = {
	{"A", "A4", 150}, {"A", "B", 150}, {"A4", "B", 150}, {"C", "A", 150}, {"C", "A4", 150}, {"C", "B", 150},
	{"C4", "C", 150}, {"Cp", "C", 150}, {"Cp", "C4", 150}, {"E", "A", 60}, {"E", "A4", 60}, {"E", "B", 60},
	{"E", "C", 60}, {"E", "C4", 60}, {"E", "Cp", 60},
}

local function step()
	local t = t_now()
	local now = time_global()

	-- The level's own creatures go offline before anything is placed, and stay so.
	sweep(now)

	if stage == "wait" and t >= 1 then
		setup_trials()
		local failed = layout()
		if failed then
			say("void layout=%s", failed)
			finish("void " .. failed)
			return
		end
		stage = "spawned"
	elseif stage == "spawned" and t >= 5 then
		local a1 = online(role_ids.A1)
		if a1 then
			hit_from_actor(a1, 0.05)
			say("event hit role=A1 npc=%d", a1:id())
		else
			say("error A1_offline")
		end
		stage = "hit_a"
	elseif stage == "hit_a" and t >= 20 then
		local c1 = online(role_ids.C1)
		if c1 then
			hit_from_actor(c1, 0.05)
			say("event hit role=C1 npc=%d", c1:id())
		else
			say("error C1_offline")
		end
		stage = "hit_c"
	elseif stage == "hit_c" and t >= 35 then
		local done = run_trials(now)
		if done or t >= TRIALS_LATEST then
			if not done then say("trials_cut index=%d of=%d", trial_index, #trials) end
			trial = nil
			-- S1's burst once E has heard its last step, S2's suppressed one after it; their weapons come out first.
			gunners.S1.ready_at, gunners.S1.fire_at = t, t + 1
			gunners.S2.ready_at, gunners.S2.fire_at = t + 2, t + 3.5
			stage = "trials_done"
		end
	elseif stage == "trials_done" and t >= 62 then
		local failed = night_layout()
		if failed then say("error night_layout=%s", failed) end
		stage = "night_wait"
	elseif stage == "night_wait" and t >= 65 then
		jump_to_night()
		observer_face_target()
		stage = "night"
	elseif stage == "night" and t >= 75 then
		torch_on()
		-- A T that never came online must not hold the run past its done line.
		local target = online(ids.T[1])
		if (torch_given and target and target:object("device_torch")) or t >= 78 then stage = "torch" end
	elseif stage == "torch" then
		torch_on()
		if t >= 78 then
			observer_face_flash()
			stage = "flash"
		end
	elseif stage == "flash" and t >= 90 then
		stage = "end"
		final()
	end

	if stage ~= "wait" then
		for _, role in ipairs({"S1", "S2", "F"}) do update_gunner(role, t) end
		close_flash_window(t)
		prepare_target()
		prepare_observer()
		log_radios()
	end
	-- A sim NPC's items come online after it and DA lights a carried torch at night (r1g: T was found by its own
	-- torch 5 s after dark), so T stays stripped until its torch is handed over, and F for good (a lit F is seen, and a
	-- flash from a shooter in sight is no news).
	if target_ready and not torch_given then
		local target = online(ids.T[1])
		local released = target and strip_torches(target) or 0
		if released > 0 then say("torch_stripped id=%d n=%d", target:id(), released) end
	end
	local f = online(ids.F[1])
	if f and f:alive() then
		local released = strip_torches(f)
		if released > 0 then say("torch_stripped id=%d role=F n=%d", f:id(), released) end
	end
	if now >= next_poll and stage ~= "wait" then
		next_poll = now + 100
		poll(now)
	end
	if now >= next_layout and stage ~= "wait" and t < 62 then
		next_layout = now + 1000
		check_layout({A = ids.A, A4 = ids.A4, B = ids.B, C = ids.C, C4 = ids.C4, Cp = ids.Cp, E = ids.E},
			LAYOUT_PAIRS, 225)
	end
	-- B stays in combat: a fresh mutant whenever the old ones are dead.
	if stage ~= "wait" and t < 60 and spots.vids and now >= next_b_bait then
		next_b_bait = now + 5000
		local b1 = online(ids.B[1])
		if b1 and not b1:best_enemy() then
			local m_vid = find_spot(spots.vids.B, {15, 10, 20}, math.random(0, 35) * 10)
			if m_vid then spawn_mutant(m_vid) end
		end
	end
end

local function on_first_update()
	cmd("g_pause_in_background 0")
	cmd("main_menu off")
	unpause()
	cmd("g_god 1")
	cmd("ai_log_period 5000")
	t0 = time_global()
	say("start scenario=ai_fair level=%s save_time=%02d:%02d weather=%s actor_comm=%s", level.name(),
		level.get_time_hours(), level.get_time_minutes(), tostring(level.get_weather()),
		db.actor:character_community())
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
end
