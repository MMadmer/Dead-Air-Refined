-- The actor menu's sort and filter row, in the real game: the inventory, a box's loot, a trade and
-- an upgrade window, in the widescreen layout and again after a switch to a 4:3 mode.
--
--   tools\qa\dx12\Invoke-Dx12Stand.ps1 -Name inv-tools -Scenario actor_menu_tools -ExtraArgs '-always_active'
--       -WindowShots -ExtraModules xrGame -Seconds 120
--
-- Asserted: sorting a pane keeps every round and every bottle and leaves the fewest boxes and
-- stacks (the player, a box; a trader keeps his sections and his ammo); filters change nothing in any
-- inventory; Take All under a filter takes only what the grid shows, and everything under "All";
-- the upgrade window gets the filters and no sort; the filter is back to "all" when the window closes
-- and after a resolution change. The trader is spawned beside the player and held there, so the
-- windows' 3 m distance check keeps them open.
local frames = 0
local passed, total = 0, 0

local function cmd(s) get_console():execute(s) end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	printf("[qa] %s %s %s", step, ok and "PASS" or "FAIL", tostring(detail or ""))
	cmd("flush")
end

-- The frame is grabbed from outside some time after its marker: nothing changes on screen for a
-- while after each shot (SETTLE).
local shots = 0
local function shot(name)
	shots = shots + 1
	printf(string.format("[qa] SHOT %02d_%s", shots, name))
	cmd("flush")
end
local SETTLE = 1100

-- A step runs once the previous one is `wait` ms old and, when it has one, its condition holds.
local steps = {}
local function step(wait, fn, cond) steps[#steps + 1] = { wait = wait, fn = fn, cond = cond } end

local function menu() return ActorMenu.get_actor_menu() end
local function mode() local am = menu(); return am and am:IsShown() and am:GetMenuMode() or 0 end
local function close_menu()
	local am = menu()
	if am and am:IsShown() then am:HideDialog() end
end

local FAMILY = { vodka = 1, vodka_2 = 2, vodka_3 = 3 }
local function stock(owner, is_box)
	local t = { items = 0, rounds = {}, boxes = {}, vodka = 0, sections = {} }
	local function add(item)
		local s = item:section()
		t.items = t.items + 1
		t.sections[s] = (t.sections[s] or 0) + 1
		if FAMILY[s] then t.vodka = t.vodka + FAMILY[s] end
		if IsAmmo(item) then
			t.rounds[s] = (t.rounds[s] or 0) + item:ammo_get_count()
			t.boxes[s] = (t.boxes[s] or 0) + 1
		end
	end
	if is_box then
		owner:iterate_inventory_box(function(_, item) add(item) end, owner)
	else
		owner:iterate_inventory(function(_, item) add(item) end, owner)
	end
	return t
end

local AMMO = "ammo_9x18_fmj"
local function box_size() return system_ini():r_float_ex(AMMO, "box_size") or 1 end

local function give(id, section, count)
	local se = alife():object(id)
	for _ = 1, count or 1 do
		alife():create(section, se.position, se.m_level_vertex_id, se.m_game_vertex_id, id)
	end
end

local function split_ammo(owner, is_box)
	local n = 5
	local function cut(item)
		if item:section() == AMMO then
			item:ammo_set_count(n)
			n = n + 6
		end
	end
	if is_box then
		owner:iterate_inventory_box(function(_, item) cut(item) end, owner)
	else
		owner:iterate_inventory(function(_, item) cut(item) end, owner)
	end
end

-- A button of the player's row: 0 sort, 1..7 the filters. The row is flush left over the bag grid
-- (x, y of dragdrop_bag in both layouts), 8 units above its old top, sort first, then the filters.
local function row_button(index)
	local dev = device()
	local kx = (dev.height / dev.width) / (768 / 1024)
	local wide = dev.width / dev.height > 1024 / 768 + 0.01
	local x = wide and 17 or 702
	local cx = index == 0 and x + 13 * kx or x + (38 + (index - 1) * 29 + 13) * kx
	return vector2():set(cx, 119 - 8 + 13)
end

local box_id, npc_id
local before, hold

-- 1. A box beside the player, and the player's own partial boxes and loose bottles.
step(0, function()
	local a = db.actor
	give(a:id(), AMMO, 3)
	give(a:id(), "vodka", 3)
	local pos = a:position()
	local se_box = alife():create("inventory_box", vector():set(pos.x + 1.2, pos.y, pos.z), a:level_vertex_id(), a:game_vertex_id())
	box_id = se_box and se_box.id
	if box_id then
		for _, s in ipairs({ AMMO, AMMO, "medkit", "medkit", "bandage", "bread", "wpn_pm", "vodka" }) do
			alife():create(s, se_box.position, se_box.m_level_vertex_id, se_box.m_game_vertex_id, box_id)
		end
	end
	check("setup-box", box_id ~= nil, tostring(box_id))
end)
step(2000, function()
	split_ammo(db.actor)
	local box = level.object_by_id(box_id)
	if box then split_ammo(box, true) end
end)

-- 2. The inventory, sorted.
step(300, function()
	before = stock(db.actor)
	get_hud():ShowActorMenu()
end)
step(1300, function()
	check("inventory-open", mode() == 1, tostring(mode()))
	check("inventory-sort-call", menu():sort_items(0), "")
end)
step(1300, function()
	local after = stock(db.actor)
	local rounds, boxes = after.rounds[AMMO] or 0, after.boxes[AMMO] or 0
	check("inventory-sort-rounds-kept", rounds == (before.rounds[AMMO] or 0), string.format("%d -> %d", before.rounds[AMMO] or 0, rounds))
	check("inventory-sort-fewest-boxes", boxes == math.ceil(rounds / box_size()), string.format("%d boxes for %d rounds", boxes, rounds))
	check("inventory-sort-bottles-kept", after.vodka == before.vodka, string.format("%d -> %d", before.vodka, after.vodka))
	shot("inv_16x9_sorted")
end)
step(SETTLE, function() SetCursorPosition(row_button(0)) end)
step(1800, function() shot("inv_16x9_hint_sort") end)
step(SETTLE, function()
	SetCursorPosition(vector2():set(512, 20))
	close_menu()
end)

-- 3. The box: Take All under the medicine filter, then under "all".
step(800, function()
	local box = level.object_by_id(box_id)
	before = stock(box, true)
	box:use(db.actor)
end)
step(1300, function()
	check("loot-open", mode() == 4, tostring(mode()))
	menu():set_item_filter(4)
end)
step(700, function() shot("loot_16x9_medicine") end)
step(SETTLE, function()
	before.actor = stock(db.actor)
	check("loot-take-all-call", menu():take_all(), "")
end)
step(1300, function()
	local box, mine = stock(level.object_by_id(box_id), true), stock(db.actor)
	local meds = (before.sections.medkit or 0) + (before.sections.bandage or 0)
	check("take-all-filtered-took-matching",
		(box.sections.medkit or 0) + (box.sections.bandage or 0) == 0 and mine.items - before.actor.items == meds,
		string.format("took %d of %d medicine", mine.items - before.actor.items, meds))
	check("take-all-filtered-left-the-rest", box.items == before.items - meds,
		string.format("box %d -> %d", before.items, box.items))
	shot("loot_16x9_after_take_filtered")
end)
step(SETTLE, function() menu():set_item_filter(0) end)
step(700, function() shot("loot_16x9_rest_all") end)
step(SETTLE, function() check("loot-take-all-call-2", menu():take_all(), "") end)
step(1300, function()
	local box = stock(level.object_by_id(box_id), true)
	check("take-all-unfiltered-took-everything", box.items == 0, tostring(box.items))
	shot("loot_16x9_empty")
end)
step(SETTLE, close_menu)

-- 4. A trader, spawned beside the player and held there.
step(500, function()
	local a = db.actor
	local pos, vid = a:position(), a:level_vertex_id()
	local sq = alife():create("stalker_sim_squad_novice", pos, vid, a:game_vertex_id())
	if sq then
		sq:create_npc(nil, pos, vid, a:game_vertex_id())
		sq.__lock = true
		for k in sq:squad_members() do
			if not npc_id then npc_id = k.id else sq:remove_npc(k.id) end
		end
	end
	check("setup-trader", npc_id ~= nil, tostring(npc_id))
	hold = npc_id
end)
step(300, function()
	local npc = level.object_by_id(npc_id)
	pcall(function() npc:force_set_goodwill(5000, db.actor) end)
	give(npc_id, AMMO, 3)
	give(npc_id, "vodka", 2)
end, function() return npc_id and level.object_by_id(npc_id) ~= nil end)
step(1000, function()
	split_ammo(level.object_by_id(npc_id))
	level.object_by_id(npc_id):start_trade(db.actor)
end)
step(1300, function()
	check("trade-open", mode() == 2, tostring(mode()))
	shot("trade_16x9_open")
end)
step(SETTLE, function() menu():set_item_filter(1) end)
step(700, function() shot("trade_16x9_weapons") end)
step(SETTLE, function()
	menu():set_item_filter(0)
	before = stock(level.object_by_id(npc_id))
	check("trade-sort-call", menu():sort_items(2), "")
end)
step(1300, function()
	local after = stock(level.object_by_id(npc_id))
	check("trader-keeps-sections", (after.sections.vodka or 0) == (before.sections.vodka or 0),
		string.format("vodka %d -> %d", before.sections.vodka or 0, after.sections.vodka or 0))
	check("trader-rounds-kept", (after.rounds[AMMO] or 0) == (before.rounds[AMMO] or 0),
		string.format("%d -> %d", before.rounds[AMMO] or 0, after.rounds[AMMO] or 0))
	-- A living NPC's ammo is not merged: his infinite ammo would replace every emptied box.
	check("trader-ammo-untouched", (after.boxes[AMMO] or 0) == (before.boxes[AMMO] or 0),
		string.format("%d -> %d boxes", before.boxes[AMMO] or 0, after.boxes[AMMO] or 0))
	check("trade-still-open", mode() == 2, tostring(mode()))
	shot("trade_16x9_sorted")
end)
step(SETTLE, close_menu)

-- 5. The upgrade window with the same partner: filters, no sort.
step(800, function() level.object_by_id(npc_id):start_upgrade(db.actor) end)
step(1300, function()
	check("upgrade-open", mode() == 3, tostring(mode()))
	check("upgrade-sort-refused", menu():sort_items(0) == false, "")
	shot("upgrade_16x9")
end)
step(SETTLE, function() menu():set_item_filter(3) end)
step(700, function() shot("upgrade_16x9_food") end)
step(SETTLE, close_menu)

-- 6. The same windows in a 4:3 mode; the menu is built anew for it.
local mode_before
step(800, function()
	mode_before = get_console():get_string("vid_mode")
	cmd("vid_mode 1024x768 (60Hz)")
	cmd("vid_restart")
end)
step(4000, function()
	check("filter-after-reset", menu():get_item_filter() == 0, tostring(menu():get_item_filter()))
	get_hud():ShowActorMenu()
	local box = alife():object(box_id)
	for _, sec in ipairs({ AMMO, "bread", "medkit" }) do
		alife():create(sec, box.position, box.m_level_vertex_id, box.m_game_vertex_id, box_id)
	end
end)
step(1300, function() check("inventory-4x3-open", mode() == 1, tostring(mode())) shot("inv_4x3") end)
step(SETTLE, function() SetCursorPosition(row_button(4)) end)
step(1800, function() shot("inv_4x3_hint_food") end)
step(SETTLE, function()
	SetCursorPosition(vector2():set(512, 20))
	close_menu()
end)
step(800, function() level.object_by_id(npc_id):start_trade(db.actor) end)
step(1300, function() check("trade-4x3-open", mode() == 2, tostring(mode())) shot("trade_4x3") end)
step(SETTLE, close_menu)
step(800, function() level.object_by_id(npc_id):start_upgrade(db.actor) end)
step(1300, function() check("upgrade-4x3-open", mode() == 3, tostring(mode())) shot("upgrade_4x3") end)
step(SETTLE, close_menu)
step(800, function() level.object_by_id(box_id):use(db.actor) end)
step(1300, function() check("loot-4x3-open", mode() == 4, tostring(mode())) shot("loot_4x3") end)
step(SETTLE, function()
	close_menu()
	if mode_before and mode_before ~= "" then
		cmd("vid_mode " .. mode_before)
		cmd("vid_restart")
	end
end)
step(2500, function()
	printf(string.format("[qa] DONE %d/%d", passed, total))
	printf("DA_WATER_PROBE_DONE")
	cmd("flush")
end)

local current, t0 = 1, 0
local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end

	-- The trader stays within the windows' reach.
	local npc = hold and level.object_by_id(hold)
	if npc and npc:position():distance_to(db.actor:position()) > 1.5 then
		local p = db.actor:position()
		npc:set_npc_position(vector():set(p.x + 1, p.y, p.z))
	end

	local s = steps[current]
	if not (s) then return end
	local now = time_global()
	if (current == 1 or now - t0 > s.wait) then
		if s.cond and not s.cond() and now - t0 < 8000 then return end
		local ok, err = pcall(s.fn)
		if not ok then check("step-" .. current, false, err) end
		t0 = now
		current = current + 1
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
