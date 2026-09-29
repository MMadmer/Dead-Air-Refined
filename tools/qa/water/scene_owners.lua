-- Several modules play scenes on the same hands: the backpack held while the inventory is open,
-- FDDA item uses, the wear scenes, the 3D PDA's own rig. This probe drives the real functions and
-- asserts that none of them ends or buries another's scene.
--
--   tools\qa\Run-WaterQa.ps1 -Name scenes -Scenario scene_owners -Save <save> -KeepShaderCache
--        -PreCommands "dar_instant_inventory 1"      (window frames: tools\qa\Watch-QaShots.ps1)
local stage, t0, frames = 0, 0, 0
local passed, total = 0, 0
local shots = 0
local held_id, rig, food, eat_at = 0, nil, nil, 0
local seen, seen_list, seen_shot = {}, {}, false

local function cmd(s) get_console():execute(s) end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	printf("[qa] %s %s %s", step, ok and "PASS" or "FAIL", tostring(detail or ""))
	cmd("flush")
end

local function shot()
	shots = shots + 1
	printf(string.format("[qa] SHOT %02d", shots))
	cmd("flush")
end

-- The inventory key, exactly as the engine takes it.
local function inventory_key()
	local am = ActorMenu.get_actor_menu()
	if am and not am:IsShown() then
		if _G.da_before_inventory() then get_hud():ShowActorMenu() end
	else
		get_hud():ShowActorMenu()
	end
end

local function menu_open()
	local am = ActorMenu.get_actor_menu()
	return am ~= nil and am:IsShown()
end

local function hands_rig()
	local outfit = db.actor:item_in_slot(7)
	return outfit and system_ini():r_string_ex(outfit:section(), "player_hud_section") or "actor_hud"
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		rig = hands_rig()
		printf("[qa] hands rig %s", tostring(rig))
		inventory_key()
		t0 = now stage = 1
	elseif stage == 1 and after(2500) then
		held_id = game.hud_scene_id()
		check("backpack-held", held_id ~= 0 and menu_open(), string.format("scene #%d, menu %s", held_id, tostring(menu_open())))
		shot()
		-- A compatible rig under the held scene (an outfit put on in the inventory): the scene goes on.
		local other = (rig == "actor_hud") and "actor_hud_cs1" or "actor_hud"
		if system_ini():section_exist(other) then
			cmd("qa_hands_rig " .. other)
		end
		t0 = now stage = 2
	elseif stage == 2 and after(600) then
		check("scene-survives-outfit-rig", game.hud_scene_id() == held_id and game.scene_active(),
			string.format("scene #%d active %s", game.hud_scene_id(), tostring(game.scene_active())))
		shot()
		-- The 3D PDA's rig carries none of the backpack motions: the scene ends instead of standing
		-- in the bind pose with the bag in front of the camera.
		cmd("qa_hands_rig pda3d_actor_hud")
		t0 = now stage = 3
	elseif stage == 3 and after(600) then
		check("scene-ends-on-foreign-rig", game.hud_scene_id() == 0, string.format("scene #%d", game.hud_scene_id()))
		shot()
		cmd("qa_hands_rig " .. rig)
		cmd("qa_hands_state")
		t0 = now stage = 4
	elseif stage == 4 and after(800) then
		-- The backpack gave the hands up; closing the window must not replay anything.
		if menu_open() then get_hud():HideActorMenu() end
		t0 = now stage = 5
	elseif stage == 5 and after(1500) then
		cmd("qa_hands_state")
		check("clean-after-foreign-rig", game.hud_scene_id() == 0 and not game.only_movekeys_allowed(),
			string.format("scene #%d movekeys %s", game.hud_scene_id(), tostring(game.only_movekeys_allowed())))
		-- Next: an item used from the open inventory while the backpack is held.
		local pos, lv, gv = db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id()
		alife():create("bread", pos, lv, gv, db.actor:id())
		inventory_key()
		t0 = now stage = 6
	elseif stage == 6 and after(2500) then
		held_id = game.hud_scene_id()
		food = db.actor:object("bread")
		check("backpack-held-again", held_id ~= 0 and menu_open() and food ~= nil,
			string.format("scene #%d, menu %s, bread %s", held_id, tostring(menu_open()), tostring(food ~= nil)))
		if food then db.actor:eat(food) end
		eat_at = now
		seen[held_id] = true
		seen_list[1] = held_id
		t0 = now stage = 7
	elseif stage == 7 then
		-- Every scene the hands go through, in order.
		local id = game.hud_scene_id()
		if id ~= 0 and not seen[id] then
			seen[id] = true
			seen_list[#seen_list + 1] = id
			printf(string.format("[qa] scene #%d at +%d ms", id, now - eat_at))
		end
		if after(1800) and not seen_shot then
			seen_shot = true
			shot()
		end
		if after(6000) then
			-- held -> the backpack put away -> the bread: three scenes, each after the other ended.
			check("item-use-after-backpack", #seen_list >= 3, "scenes " .. table.concat(seen_list, ","))
			t0 = now stage = 8
		end
	elseif stage == 8 and after(3000) then
		cmd("qa_hands_state")
		check("clean-after-item-use", game.hud_scene_id() == 0 and not game.only_movekeys_allowed(),
			string.format("scene #%d movekeys %s", game.hud_scene_id(), tostring(game.only_movekeys_allowed())))
		printf(string.format("[qa] DONE %d/%d", passed, total))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 9
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
