-- The scene bag for every kind of backpack: the slot emptied or given each backpack in turn, the
-- inventory opened and closed the way the key does it, every pass recorded by r__capture (the scene
-- without the UI, so the bag shows under the inventory panel). Frames land in capture_* folders in
-- the order of the [qa] BAG lines (tools\qa\Run-WaterQa.ps1 -Scenario bags).
-- The first pass keeps the suit the save has on (a suit may lock the slot - the cheap default bag is
-- what the player sees then); the suit goes into the backpack for the rest, so every backpack fits.
local TARGETS = { "", "", "kit_hunt", "backpack_light", "backpack_heavy", "backpack_travel", "backpack_stalker",
	"exobackpack" }
local BACKPACK_SLOT = 15
local stage, t0, frames, idx = 0, 0, 0, 0
local passed, total = 0, 0

local function cmd(s) get_console():execute(s) end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	printf("[qa] %s %s %s", step, ok and "PASS" or "FAIL", tostring(detail or ""))
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

local function slot_section()
	local it = db.actor:item_in_slot(BACKPACK_SLOT)
	return it and it:section() or ""
end

local function expected_visual(sect)
	local ini = system_ini()
	local s = (sect ~= "" and ini:section_exist("da_backpack_" .. sect .. "_hud")) and ("da_backpack_" .. sect .. "_hud")
		or "da_backpack_hud"
	return s, ini:r_string_ex(s, "item_visual") or "?"
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		idx = idx + 1
		if idx > #TARGETS then
			printf(string.format("[qa] DONE %d/%d", passed, total))
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			stage = 9
			return
		end
		if idx == 1 then
			local suit = db.actor:item_in_slot(7)
			local sect = suit and suit:section() or ""
			printf("[qa] suit [" .. sect .. "] backpack slot open " ..
				tostring(sect == "" or not system_ini():line_exist(sect, "backpack_avaliable")
					or system_ini():r_bool(sect, "backpack_avaliable")))
		elseif idx == 2 then
			local suit = db.actor:item_in_slot(7)
			if suit then db.actor:move_to_ruck(suit) end
		end
		-- the slot emptied, then the next backpack made and put in
		local cur = db.actor:item_in_slot(BACKPACK_SLOT)
		if cur then db.actor:move_to_ruck(cur) end
		local target = TARGETS[idx]
		if target ~= "" then
			alife():create(target, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(),
				db.actor:id())
		end
		t0 = now stage = 1
	elseif stage == 1 and after(idx == 2 and 2500 or 700) then
		local target = TARGETS[idx]
		if target ~= "" then
			local best
			db.actor:iterate_inventory(function(_, item)
				if item:section() == target and (not best or item:id() > best:id()) then best = item end
			end, db.actor)
			if best then db.actor:move_to_slot(best, BACKPACK_SLOT) end
		end
		t0 = now stage = 2
	elseif stage == 2 and after(700) then
		local target = TARGETS[idx]
		local sect, visual = expected_visual(slot_section())
		check("slot-" .. (target ~= "" and target or "empty"), slot_section() == target,
			"slot [" .. slot_section() .. "] scene [" .. sect .. "] visual [" .. visual .. "]")
		printf(string.format("[qa] BAG %d %s %s", idx, target ~= "" and target or "empty", visual))
		cmd("r__capture 5 15")
		inventory_key()
		t0 = now stage = 3
	elseif stage == 3 and after(3500) then
		check("held-" .. idx, game.hud_scene_id() ~= 0, "scene #" .. game.hud_scene_id() .. " menu " .. tostring(menu_open()))
		inventory_key()
		t0 = now stage = 4
	elseif stage == 4 and after(2600) then
		check("put-away-" .. idx, game.hud_scene_id() == 0 and not menu_open(),
			"scene #" .. game.hud_scene_id() .. " menu " .. tostring(menu_open()))
		t0 = now stage = 0
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
