-- Spent cases of NPCs and the kick: a hostile soldier a dozen metres off fires at the player (god mode), his cases
-- are counted; then the player's own burst, and a walk through those cases nudges them.
--
--   tools\qa\Run-WaterQa.ps1 -Name shellsnpc -Scenario shells_npc -Save puddle -KeepShaderCache -Seconds 150
local stage, t0, frames = 0, 0, 0
local npc_id

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function act(name, on)
	if on then level.press_action(level.action_id(name)) else level.release_action(level.action_id(name)) end
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		cmd("g_god on")
		local d = device().cam_dir
		local p = db.actor:position()
		local lv = level.vertex_in_direction(db.actor:level_vertex_id(), vector():set(d.x, 0, d.z):normalize(), 12)
		local se = alife():create("sim_default_military_1", level.vertex_position(lv), lv, db.actor:game_vertex_id())
		npc_id = se and se.id
		say("[qa] npc spawned " .. tostring(npc_id) .. " at vertex " .. tostring(lv))
		for _, sec in ipairs({ "wpn_ak74", "ammo_5.45x39_fmj" }) do
			alife():create(sec, p, db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		end
		t0 = now stage = 1
	elseif stage == 1 and (after(10000) or (npc_id and level.object_by_id(npc_id))) then
		local npc = npc_id and level.object_by_id(npc_id)
		if npc then
			npc:set_relation(game_object.enemy, db.actor)
			db.actor:set_relation(game_object.enemy, npc)
		end
		say("[qa] npc " .. tostring(npc and npc:section()) .. " online " .. tostring(npc ~= nil) .. " after " .. (now - t0) .. " ms")
		db.actor:iterate_inventory(function(_, item)
			if item:section() == "wpn_ak74" then db.actor:move_to_slot(item, 3) end
		end, nil)
		t0 = now stage = 2
	elseif stage == 2 and after(12000) then
		cmd("qa_shells")
		local npc = npc_id and level.object_by_id(npc_id)
		if npc then
			local se = alife_object(npc_id)
			if se then alife():release(se, true) end
		end
		db.actor:activate_slot(3)
		t0 = now stage = 3
	elseif stage == 3 and after(2500) then
		act("wpn_fire", true)
		t0 = now stage = 4
	elseif stage == 4 and after(500) then
		act("wpn_fire", false)
		t0 = now stage = 5
	elseif stage == 5 and after(2500) then
		cmd("qa_shells")
		-- a step and a half before the cases, then straight through them
		cmd("qa_shells goto 1.5")
		t0 = now stage = 51
	elseif stage == 51 and after(500) then
		t0 = now stage = 6
	elseif stage == 6 then
		-- the forward key held for a second and a half: the player walks through his cases
		cmd("qa_shells walk")
		if after(1500) then t0 = now stage = 7 end
	elseif stage == 7 and after(2000) then
		cmd("qa_shells")
		cmd("g_god off")
		say("DA_WATER_PROBE_DONE")
		cmd("quit")
		stage = 8
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
