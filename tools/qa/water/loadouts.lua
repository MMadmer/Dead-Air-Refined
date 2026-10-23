-- The debug menu's loadouts (docs/dead-air/DEBUG_MENU.md) in one short run: a set given (worn what it wears, the rest in
-- the inventory), a set replacing the inventory (the base equipment kept), a set created from the worn gear, deleted,
-- a standard one deleted and restored. Each check prints "[qa] lo <name> PASS|FAIL <detail>"; a shot follows the gives.
--
--   tools\qa\Run-WaterQa.ps1 -Name loadouts -Scenario loadouts -Save qa_meadow -KeepShaderCache -Seconds 120
--   qa_lo_hold = 15   seconds the window stays open on the last step, announced as "[qa] lo READY"
local stage, t0, frames, step = "start", 0, 0, 0
local passed, total = 0, 0

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end
local function dbg(s) cmd("dar_debug_menu_do " .. s) end

local function check(name, ok, detail)
	total = total + 1
	if ok then passed = passed + 1 end
	say("[qa] lo " .. name .. " " .. (ok and "PASS" or "FAIL") .. " " .. tostring(detail or ""))
end

local function slot(i)
	local it = db.actor:item_in_slot(i)
	return it and it:section() or "-"
end

local function count_of(sec)
	local n = 0
	db.actor:iterate_inventory(function(_, item) if item:section() == sec then n = n + 1 end end, nil)
	return n
end

local function total_items()
	local n = 0
	db.actor:iterate_inventory(function() n = n + 1 end, nil)
	return n
end

local function slots()
	return "outfit " .. slot(7) .. " helmet " .. slot(12) .. " backpack " .. slot(15) .. " rifles " .. slot(3) .. "/" .. slot(2) ..
		" pistol " .. slot(5) .. " knife " .. slot(1) .. " detector " .. slot(9) .. " binoc " .. slot(14)
end

local function file_has(id)
	local f = ini_file("$app_data_root$", "dead_air_x64_loadouts.ltx")
	return f:section_exist(id) and f:line_exist(id, "items") and not f:line_exist(id, "deleted")
end

local plan = {
	function() cmd("g_god on"); cmd("dar_debug_menu 1"); cmd("dar_debug_menu_open") end,
	function() dbg("tab loadouts"); dbg("state") end,
	function() dbg("pick_key scientist"); dbg("choice 1"); dbg("press primary") end,
	function()
		say("[qa] lo slots " .. slots())
		check("give_worn", slot(7) == "scientific_outfit_backpack" and slot(15) == "backpack_travel" and slot(3) == "wpn_spas12" and slot(2) == "wpn_ak74",
			slots())
		check("give_ruck", count_of("medkit") >= 3 and count_of("geiger") >= 1 and count_of("ammo_5.45x39_ap") >= 5
			and count_of("ammo_12x70_buck") >= 5 and count_of("ammo_9x18_pmm") >= 5,
			"medkit " .. count_of("medkit") .. " geiger " .. count_of("geiger") .. " 5.45 " .. count_of("ammo_5.45x39_ap") ..
			" 12x70 " .. count_of("ammo_12x70_buck") .. " 9x18 " .. count_of("ammo_9x18_pmm"))
		cmd("r__screenshot_every 1")
	end,
	function() cmd("r__screenshot_every 0"); dbg("pick_key heavy"); dbg("choice 2"); dbg("press primary") end,
	function()
		say("[qa] lo slots " .. slots())
		check("replace_worn", slot(7) == "exo_outfit" and slot(15) == "backpack_heavy" and slot(3) == "wpn_groza" and slot(5) == "wpn_desert_eagle", slots())
		check("replace_cleared", count_of("scientific_outfit_backpack") == 0 and count_of("medkit") == 0, "seva " ..
			count_of("scientific_outfit_backpack") .. " medkit " .. count_of("medkit") .. " total " .. total_items())
		check("replace_kept", count_of("wpn_upd") + count_of("device_pda") + count_of("device_torch") >= 2,
			"upd " .. count_of("wpn_upd") .. " pda " .. count_of("device_pda") .. " torch " .. count_of("device_torch"))
		cmd("r__screenshot_every 1")
	end,
	function() cmd("r__screenshot_every 0"); dbg("pick_key #new"); dbg("press primary") end,
	function()
		local f = ini_file("$app_data_root$", "dead_air_x64_loadouts.ltx")
		local id
		f:section_for_each(function(s) if string.find(s, "^own_") and f:line_exist(s, "items") and not f:line_exist(s, "deleted") then id = s end end)
		say("[qa] lo created " .. tostring(id) .. " items " .. tostring(id and f:r_string_ex(id, "items")))
		check("create", id ~= nil and string.find(f:r_string_ex(id, "items") or "", "exo_outfit", 1, true) ~= nil, tostring(id))
		dbg("state")
		_G.qa_lo_created = id
		if id then dbg("pick_key " .. id); dbg("press third") end
	end,
	function()
		check("delete_own", not file_has(_G.qa_lo_created or "own_1"), tostring(_G.qa_lo_created))
		dbg("pick_key wanderer"); dbg("press third")
	end,
	function()
		local f = ini_file("$app_data_root$", "dead_air_x64_loadouts.ltx")
		check("delete_shipped", f:section_exist("hidden") and f:line_exist("hidden", "wanderer"), "")
		dbg("pick_key #restore"); dbg("press primary")
	end,
	function()
		local f = ini_file("$app_data_root$", "dead_air_x64_loadouts.ltx")
		check("restore", not (f:section_exist("hidden") and f:line_exist("hidden", "wanderer")), "")
		dbg("pick_key scientist")
		dbg("state")
		-- the window held open for a capture of it (Capture-HiddenDesktopWindow.ps1 on "[qa] lo READY")
		if qa_lo_hold then say("[qa] lo READY") end
	end,
}

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	if stage == "start" then
		stage, t0 = "run", now
	elseif stage == "run" and now - t0 > ((step == #plan and qa_lo_hold) and qa_lo_hold * 1000 or 2500) then
		step = step + 1
		local f = plan[step]
		if not f then
			say(string.format("[qa] lo %d/%d passed", passed, total))
			cmd("dar_debug_menu_do close")
			say("DA_WATER_PROBE_DONE")
			stage = "end"
			return
		end
		local ok, err = pcall(f)
		if not ok then say("[qa] lo step " .. step .. " ERROR " .. tostring(err)) end
		t0 = now
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
