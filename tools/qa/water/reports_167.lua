-- Player reports of 1.6.6, one run:
--  * a hud section naming a cycle the hands lack (a PDA addon's kpk_idle) - the item is made without it; a weapon
--    whose anm_show*/anm_hide* name missing cycles (broken BEFORE its hud item exists, so the pool takes the broken
--    section - "[qa] hud motion [anm_show] ... def none" proves it) is still drawn and holstered: "[qa] draw state 0",
--    "[qa] holster state 3";
--  * the transitions' map hints survive a UI reset (here g_language, the same reset a resolution or style change
--    makes): "[qa] map spot [level_changer...]" lines read the destinations after the reset, never "no hint";
--  * the arena crash - the actor's PDA moved to a box while an online stalker is its contact, then that stalker is
--    released: the run has to reach the marker.
--
--   tools\qa\Run-WaterQa.ps1 -Name reports167 -Scenario reports_167 -Save glowsq -KeepShaderCache -Seconds 90
local stage, frames, wait_until = "start", 0, 0
local npc_id, box_id, pistol_id, pda
-- a gun nobody near the rig carries, so its hud item cannot be in the pool already
local qa_gun = "wpn_gauss"
local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function nearest_stalker(max_d)
	local best, best_d
	for id = 1, 65534 do
		local o = level.object_by_id(id)
		if o and IsStalker(o) and o:id() ~= db.actor:id() and o:alive() then
			local d = o:position():distance_to(db.actor:position())
			if d < max_d and (not best_d or d < best_d) then best, best_d = o, d end
		end
	end
	return best, best_d
end

local function find_pda()
	local found
	db.actor:iterate_inventory(function(_, item)
		if not found and item:section() == "device_pda" then found = item end
	end, db.actor)
	return found
end

-- every anm_show* and anm_hide* key of a hud section names a cycle no hands model has
local function break_draw(hud)
	local ini = system_ini()
	ini:set_readonly(false)
	ini:set_override_names(true)
	local keys = {}
	for i = 0, ini:line_count(hud) - 1 do
		local _, key = ini:r_line(hud, i, "", "")
		if key and (string.find(key, "^anm_show") or string.find(key, "^anm_hide")) then keys[#keys + 1] = key end
	end
	for _, key in ipairs(keys) do ini:w_string(hud, key, "qa_missing_cycle") end
	return #keys
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 150 or frames < wait_until then return end
	local sim = alife()
	if stage == "start" then
		-- the reported crash path: a hud section created with a cycle its hands lack
		-- qa_old_build (a 1.6.6 control run): the hud checks would end it before the PDA one
		if not qa_old_build then
			local ini = system_ini()
			ini:set_readonly(false)
			ini:w_string("pda_show_animator_hud", "anm_qa_kpk", "kpk_idle")
			cmd("qa_hud_motion pda_show_animator_hud anm_qa_kpk")
			say("[qa] hud pda section made")
		end
		cmd("qa_map_hints level_changer")

		local p = db.actor:position()
		local npc, d = nearest_stalker(60)
		if npc then
			npc_id = npc:id()
			say(string.format("[qa] pda contact %s at %.0f m", npc:name(), d))
		else
			local se_npc = sim:create("sim_default_stalker_0", vector():set(p.x + 4, p.y, p.z), db.actor:level_vertex_id(),
				db.actor:game_vertex_id())
			npc_id = se_npc and se_npc.id
			say("[qa] pda contact spawned " .. tostring(npc_id))
		end
		local box = sim:create("inventory_box", vector():set(p.x + 1, p.y, p.z + 1), db.actor:level_vertex_id(),
			db.actor:game_vertex_id())
		box_id = box and box.id
		if not qa_old_build then
			local hud = system_ini():r_string(qa_gun, "hud")
			say(string.format("[qa] draw cycles broken %d in %s", break_draw(hud), hud))
			cmd("qa_hud_motion " .. hud .. " anm_show")
			local pistol = sim:create(qa_gun, p, db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
			pistol_id = pistol and pistol.id
		end
		stage, wait_until = "draw", frames + 120
	elseif stage == "draw" then
		local npc = npc_id and level.object_by_id(npc_id)
		say(string.format("[qa] pda contact online %s at %.0f m", tostring(npc ~= nil),
			npc and npc:position():distance_to(db.actor:position()) or -1))
		local pistol = pistol_id and level.object_by_id(pistol_id)
		if pistol then
			db.actor:make_item_active(pistol)
		else
			say("[qa] draw no pistol")
		end
		cmd("g_language " .. (get_console():get_token("g_language") or "rus"))
		say("[qa] ui reset done")
		stage, wait_until = "check", frames + 150
	elseif stage == "check" then
		cmd("qa_map_hints level_changer")
		local pistol = pistol_id and level.object_by_id(pistol_id)
		local active = db.actor:active_item()
		say(string.format("[qa] draw state %s active %s", tostring(pistol and pistol:get_state()),
			tostring(active and active:section())))
		if pistol then db.actor:activate_slot(0) end
		pda = find_pda()
		local box = box_id and level.object_by_id(box_id)
		if pda and box then
			db.actor:transfer_item(pda, box)
			say("[qa] pda moved to the box")
		else
			say(string.format("[qa] pda %s box %s", tostring(pda), tostring(box)))
		end
		stage, wait_until = "release", frames + 30
	elseif stage == "release" then
		local pistol = pistol_id and level.object_by_id(pistol_id)
		say(string.format("[qa] holster state %s", tostring(pistol and pistol:get_state())))
		say(string.format("[qa] pda parent %s", tostring(pda and pda:parent() and pda:parent():id())))
		local se = npc_id and sim:object(npc_id)
		if se then
			sim:release(se, true)
			say("[qa] pda contact released")
		end
		stage, wait_until = "end", frames + 90
	elseif stage == "end" then
		say("[qa] still alive after the release")
		stage = "done"
		say("DA_WATER_PROBE_DONE")
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
