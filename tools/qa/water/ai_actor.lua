-- R3 ai-actor (docs\dead-air\ai\PLAN.md 7.3): the actor is the target, standing still and never shooting. God mode
-- would hide every hit on him from scripts, so instead each hit is counted and cancelled in actor_on_before_hit and
-- his health kept full: that measures settle (hits per second after identification) and keeps him alive as g_god does.
--   * army_sim_squad_advanced (4) at 80 m: identification and reaction, settle, flanks, radio latency; at t=30 s the
--     army leader (ai_is_leader()) dies by a scripted hit from the actor: who saw it, who was told, the handover;
--   * bandit_sim_squad_novice (4) at 60 m on another side: two die at t=40 s, expect a scatter;
--   * monolith_sim_squad_novice (4) at 60 m on a third side: two die at t=50 s, expect no retreat.
-- The squads are made neutral to each other and sworn enemies of the actor; every NPC stays within 200 m (C26).
--
--   tools\qa\Run-WaterQa.ps1 -Name ai-actor -Scenario ai_actor -Save fogqa -Seconds 150 `
--       -ExtraArgs "-force_flushlog -always_active" -Out "D:\Games\Dead Air\_work\ai-npc\qa"
--
-- Logged: per NPC its radio, rank and leader flag, its awareness of the actor (each 0.1 step and reaching 1), its sight
-- of him, belief changes, every shot, morale changes, combat action changes, leader flag changes; every hit on the
-- actor with its shooter; at each kill who saw the victim and from how far; reports received. Lines
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
-- the sender and how far apart the two stood.
local function on_report(receiver, target, kind, channel, sender)
	if not (t0 and receiver) or finished then return end
	say("report npc=%d role=%s comm=%s kind=%s channel=%s target=%s sender=%s d=%.0f", receiver:id(),
		roles[receiver:id()] or "-", receiver:character_community(), KINDS[(kind or -1) + 1] or tostring(kind),
		CHANNELS[(channel or -1) + 1] or tostring(channel), name_of(target), name_of(sender),
		sender and receiver:position():distance_to(sender:position()) or -1)
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
local function poll_shots(npc, extra)
	local item = npc:active_item()
	local class = item and npc:ai_weapon_class(item) or ""
	if class == "" or class == "none" then return end
	local ammo = item:get_ammo_in_magazine()
	local last = ammo_seen[npc:id()]
	if last and last.id == item:id() and ammo < last.ammo then
		local enemy = npc:best_enemy()
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
local SQUADS = {
	{name = "army", section = "army_sim_squad_advanced", d = 80, kill_at = 30, kills = "leader"},
	{name = "bandit", section = "bandit_sim_squad_novice", d = 60, kill_at = 40, kills = 2},
	{name = "monolith", section = "monolith_sim_squad_novice", d = 60, kill_at = 50, kills = 2},
}
local SQUAD_SIZE, DURATION = 4, 90

local stage = "wait"
local groups = {} -- squad name -> member ids
local group_of = {} -- id -> squad name
local next_poll, next_layout = 0, 0
local see_seen, awareness_seen, action_seen, leader_seen = {}, {}, {}, {}
local relations_forced, kills_done = {}, {}
local actor_hits = 0

local function all_members()
	local list = {}
	for _, squad in ipairs(SQUADS) do
		for _, id in ipairs(groups[squad.name] or {}) do list[#list + 1] = id end
	end
	return list
end

-- The three squads on three sides of the actor, each where it sees him.
local function layout()
	local actor_vid, actor = db.actor:level_vertex_id(), db.actor:position()
	local start = math.random(0, 35) * 10
	local placed = {}
	for index, squad in ipairs(SQUADS) do
		local d = squad.d
		local vid, pos = find_spot(actor_vid, {d, d - 5, d + 5, d - 10, d + 10}, start + (index - 1) * 120,
			function(p) return los(p, actor) and far_from(p, placed, 90) end)
		if not vid then return "no_" .. squad.name end
		placed[#placed + 1] = pos
		local _, ids = spawn_squad(squad.section, vid, SQUAD_SIZE)
		groups[squad.name] = ids
		for i, id in ipairs(ids) do
			group_of[id] = squad.name
			tag(id, squad.name .. i)
		end
		say("side name=%s section=%s members=%d x=%.1f z=%.1f from_actor=%.0f", squad.name, squad.section, #ids,
			pos.x, pos.z, flat_dist(pos, actor))
	end
end

-- Sworn enemies of the actor, neutral to the other two squads.
local function force_relations()
	for id, name in pairs(group_of) do
		local npc = not relations_forced[id] and online(id)
		if npc then
			local all = true
			npc:force_set_goodwill(-1000, db.actor)
			for other_id, other_name in pairs(group_of) do
				if other_name ~= name then
					local other = online(other_id)
					if other then
						npc:force_set_goodwill(0, other)
					else
						all = false
					end
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

local function poll()
	local actor = db.actor
	for _, id in ipairs(all_members()) do
		local npc = online(id)
		if npc and npc:alive() then
			local see = npc:see(actor)
			if see_seen[id] ~= see then
				see_seen[id] = see
				say("see npc=%d role=%s comm=%s target=actor value=%d d=%.0f", id, roles[id] or "-",
					npc:character_community(), see and 1 or 0, npc:position():distance_to(actor:position()))
			end
			local value = npc:ai_awareness(actor)
			local last = awareness_seen[id]
			if not last or math.abs(value - last) >= 0.1 or (value >= 1) ~= (last >= 1) then
				awareness_seen[id] = value
				say("awareness npc=%d role=%s target=actor value=%.2f see=%d danger=%d", id, roles[id] or "-", value,
					see and 1 or 0, npc:best_danger() and 1 or 0)
			end
			poll_belief(npc, actor, "actor")
			poll_shots(npc)
			poll_morale(npc)
			local action = combat_action(npc) or -1
			if action_seen[id] ~= action then
				action_seen[id] = action
				say("action npc=%d role=%s id=%d", id, roles[id] or "-", action)
			end
			local leader = npc:ai_is_leader()
			if leader_seen[id] ~= leader then
				leader_seen[id] = leader
				say("leader npc=%d role=%s comm=%s value=%d", id, roles[id] or "-", npc:character_community(),
					leader and 1 or 0)
			end
		end
	end
end

local function kill(npc)
	local h = hit()
	h.power = 100
	h.impulse = 0
	h.draftsman = db.actor
	h.type = hit.fire_wound
	h.direction = vector():set(npc:position()):sub(db.actor:position()):normalize()
	h:bone("bip01_head")
	npc:hit(h)
end

-- The victims of one squad; first who of the squad sees each of them and how far away it stands, then the kills.
local function kill_in(squad)
	local victims = {}
	for _, id in ipairs(groups[squad.name] or {}) do
		local npc = online(id)
		if npc and npc:alive() then
			if squad.kills == "leader" then
				if npc:ai_is_leader() then victims[1] = npc end
			elseif #victims < squad.kills then
				victims[#victims + 1] = npc
			end
		end
	end
	if #victims == 0 then
		say("error no_victim squad=%s", squad.name)
		return
	end
	for _, victim in ipairs(victims) do
		for _, id in ipairs(groups[squad.name]) do
			local mate = online(id)
			if mate and mate:alive() and id ~= victim:id() then
				say("witness npc=%d role=%s victim=%s see=%d d=%.0f", id, roles[id] or "-", roles[victim:id()] or "-",
					mate:see(victim) and 1 or 0, mate:position():distance_to(victim:position()))
			end
		end
	end
	for _, victim in ipairs(victims) do
		say("event kill role=%s npc=%d squad=%s leader=%d", roles[victim:id()] or "-", victim:id(), squad.name,
			victim:ai_is_leader() and 1 or 0)
		kill(victim)
	end
end

local function final()
	for _, id in ipairs(all_members()) do
		local npc = online(id)
		if npc and npc:alive() then dump_belief(npc, db.actor, "actor") end
	end
	say("actor_hits total=%d", actor_hits)
	flush_stats()
	finish(void_reason and ("void " .. void_reason) or "complete")
end

local function step()
	local t, now = t_now(), time_global()
	if stage == "wait" and t >= 1 then
		local failed = layout()
		if failed then
			finish("void " .. failed)
			return
		end
		log_action_ids()
		stage = "fight"
	elseif stage == "fight" then
		force_relations()
		log_radios()
		if now >= next_poll then
			next_poll = now + 100
			poll()
		end
		if now >= next_layout then
			next_layout = now + 1000
			check_layout(groups, {}, 200)
		end
		for _, squad in ipairs(SQUADS) do
			if t >= squad.kill_at and not kills_done[squad.name] then
				kills_done[squad.name] = true
				kill_in(squad)
			end
		end
		if t >= DURATION then
			stage = "end"
			final()
		end
	end
end

-- Every hit on the actor is counted with its shooter and cancelled: he stays whole without god mode.
local function on_actor_before_hit(shit, bone_id, flags)
	flags.ret_value = false
	if not t0 or finished then return end
	local who = shit.draftsman
	local id = who and who:id()
	actor_hits = actor_hits + 1
	say("actor_hit npc=%s role=%s comm=%s bone=%s power=%.2f", id and tostring(id) or "-", id and roles[id] or "-",
		id and id ~= 0 and IsStalker(who) and who:character_community() or "-", tostring(bone_id), shit.power)
end

local function on_death(victim, who)
	local id = victim and victim:id()
	if id and group_of[id] then
		say("death npc=%d role=%s comm=%s killer=%s", id, roles[id] or "-", group_of[id],
			who and tostring(who:id()) or "-")
	end
end

local function on_first_update()
	cmd("g_pause_in_background 0")
	cmd("main_menu off")
	unpause()
	cmd("g_god 0")
	cmd("ai_log_period 5000")
	t0 = time_global()
	say("start scenario=ai_actor level=%s save_time=%02d:%02d weather=%s actor_comm=%s", level.name(),
		level.get_time_hours(), level.get_time_minutes(), tostring(level.get_weather()),
		db.actor:character_community())
	if not reports_subscribed then say("error no_report_subscription") end
end

local function on_update()
	unpause()
	if not t0 then return end
	db.actor:set_health_ex(1)
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
	RegisterScriptCallback("actor_on_before_hit", on_actor_before_hit)
	RegisterScriptCallback("npc_on_death_callback", on_death)
end
