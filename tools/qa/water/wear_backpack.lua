-- A suit put on from the open inventory while the backpack is held: the wear scene waits for the
-- bag to be put away and then plays; the hands model changes under the held scene without
-- dropping it (tools\qa\Run-WaterQa.ps1 -Scenario wear_backpack, frames via Watch-QaShots.ps1).
local stage, t0, frames = 0, 0, 0
local passed, total = 0, 0
local shots = 0
local held_id, outfit, wear_at = 0, nil, 0
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

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		-- A second suit of the kind already worn, into the backpack; the worn one is taken off so
		-- the slot is free for the put-on, as the UI requires.
		local worn = db.actor:item_in_slot(7)
		local sect = worn and worn:section() or "novice_outfit"
		printf("[qa] suit %s", sect)
		if worn then db.actor:move_to_ruck(worn) end
		alife():create(sect, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		inventory_key()
		t0 = now stage = 1
	elseif stage == 1 and after(3000) then
		held_id = game.hud_scene_id()
		for _, it in ipairs({ db.actor:item_in_slot(7) }) do outfit = it end
		-- the newest copy of the suit in the backpack
		local best
		db.actor:iterate_inventory(function(_, item)
			if item:clsid() == clsid.equ_stalker_s or (IsOutfit and IsOutfit(item)) then
				if not best or item:id() > best:id() then best = item end
			end
		end, db.actor)
		outfit = best
		check("backpack-held", held_id ~= 0 and menu_open() and outfit ~= nil,
			string.format("scene #%d, menu %s, suit %s", held_id, tostring(menu_open()), tostring(outfit and outfit:section())))
		if outfit and _G.da_before_wear and _G.da_before_wear(outfit, 7) then
			db.actor:move_to_slot(outfit, 7)
		end
		seen[held_id] = true
		seen_list[1] = held_id
		wear_at = now
		t0 = now stage = 2
	elseif stage == 2 then
		local id = game.hud_scene_id()
		if id ~= 0 and not seen[id] then
			seen[id] = true
			seen_list[#seen_list + 1] = id
			printf(string.format("[qa] scene #%d at +%d ms", id, now - wear_at))
		end
		if after(1600) and not seen_shot then
			seen_shot = true
			shot()
		end
		if after(6000) then
			-- held -> the backpack put away -> the wear scene.
			check("wear-after-backpack", #seen_list >= 3, "scenes " .. table.concat(seen_list, ","))
			t0 = now stage = 3
		end
	elseif stage == 3 and after(2500) then
		cmd("qa_hands_state")
		check("clean-after-wear", game.hud_scene_id() == 0 and not game.only_movekeys_allowed() and db.actor:item_in_slot(7) ~= nil,
			string.format("scene #%d movekeys %s suit %s", game.hud_scene_id(), tostring(game.only_movekeys_allowed()),
				tostring(db.actor:item_in_slot(7) and db.actor:item_in_slot(7):section())))
		printf(string.format("[qa] DONE %d/%d", passed, total))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 4
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
