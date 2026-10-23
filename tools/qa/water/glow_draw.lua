-- The glowstick drawn with the detector key (user's save glowsq). qa_glow_plan "keys": bare-handed, then holstered,
-- then with each weapon slot in the hands, then the weapon slots again; "equip": put in its slot through the
-- inventory window and drawn as the window closes, then while it is open. Each step presses its keys (level.press_action) or activates a
-- slot, settles, logs the hands ("[qa] glowd <label> active ... detector ... state ...") and takes a shot.
--
--   tools\qa\Run-WaterQa.ps1 -Name glow-draw -Scenario glow_draw -Save glowsq -KeepShaderCache -Seconds 90
local c = get_console()
local stage, t0, frames = "start", 0, 0
local step, shots = 0, 0
local det_key

local function cmd(s) c:execute(s) end
local function say(s) printf(s) cmd("flush") end

local function press(id)
	level.press_action(id)
	level.release_action(id)
end

local function hands(label)
	local a = db.actor:active_item()
	local det = db.actor:active_detector()
	say(string.format("[qa] glowd %s active %s slot %d detector %s state %s torch %s", label, a and a:section() or "-",
		db.actor:active_slot(), det and det:section() or "-", det and tostring(det:get_state()) or "-",
		tostring(itms_manager and itms_manager.TorchType)) .. " scene " .. tostring(game.scene_active()) .. " " ..
		tostring(game.hud_scene_id()) .. " window " .. tostring(ActorMenu.get_actor_menu():IsShown()))
end

local function glowstick()
	local g
	db.actor:iterate_inventory(function(_, item) if item:section() == "device_glowstick" then g = item end end, nil)
	return g
end

-- label, action, milliseconds to settle
local plans = {}
plans.keys = {
	{ "bare_key", function() press(det_key) end, 2500 },
	{ "bare_key_again", function() press(det_key) end, 2500 },
	{ "slot2", function() db.actor:activate_slot(2) end, 2500 },
	{ "slot2_key", function() press(det_key) end, 2500 },
	{ "slot2_key_again", function() press(det_key) end, 2500 },
	{ "slot3", function() db.actor:activate_slot(3) end, 2500 },
	{ "slot3_key", function() press(det_key) end, 3000 },
	{ "slot3_key_again", function() press(det_key) end, 3000 },
	{ "slot2_after", function() db.actor:activate_slot(2) end, 2500 },
	{ "slot0_after", function() db.actor:activate_slot(0) end, 2500 },
	{ "key_after", function() press(det_key) end, 2500 },
}
-- Equipped through the inventory window (its backpack scene holds the hands), the key pressed as it closes or while it
-- is open.
plans.equip = {
	{ "to_ruck", function() db.actor:move_to_ruck(glowstick()) end, 1500 },
	{ "inv_open", function() get_hud():ShowActorMenu() end, 2500 },
	{ "equip_open", function() db.actor:move_to_slot(glowstick(), 9) end, 1500 },
	{ "inv_close_key", function() get_hud():HideActorMenu(); press(det_key) end, 3000 },
	{ "key_again", function() press(det_key) end, 3000 },
	{ "slot3", function() db.actor:activate_slot(3) end, 2500 },
	{ "slot0", function() db.actor:activate_slot(0) end, 2500 },
	{ "to_ruck2", function() db.actor:move_to_ruck(glowstick()) end, 1500 },
	{ "inv_open2", function() get_hud():ShowActorMenu() end, 2500 },
	{ "equip_open2", function() db.actor:move_to_slot(glowstick(), 9) end, 1500 },
	{ "key_in_inv", function() press(det_key) end, 3000 },
	{ "inv_close2", function() get_hud():HideActorMenu() end, 3000 },
	{ "key_after2", function() press(det_key) end, 3000 },
	{ "slot3_after2", function() db.actor:activate_slot(3) end, 2500 },
}
-- The inventory key as the engine handles it: closed, the backpack scene is asked first (it opens the window itself
-- at the end of its show); open, the window goes.
local function inv_key()
	if ActorMenu.get_actor_menu():IsShown() then
		get_hud():HideActorMenu()
	elseif _G.da_before_inventory == nil or _G.da_before_inventory() then
		get_hud():ShowActorMenu()
	end
end
plans.backpack = {
	{ "to_ruck", function() db.actor:move_to_ruck(glowstick()) end, 1500 },
	{ "inv_key", inv_key, 4000 },
	{ "equip_open", function() db.actor:move_to_slot(glowstick(), 9) end, 1500 },
	{ "inv_key_det", function() inv_key(); press(det_key) end, 4000 },
	{ "det_again", function() press(det_key) end, 3000 },
	{ "slot3", function() db.actor:activate_slot(3) end, 2500 },
	{ "slot0", function() db.actor:activate_slot(0) end, 2500 },
	{ "to_ruck2", function() db.actor:move_to_ruck(glowstick()) end, 1500 },
	{ "inv_key2", inv_key, 4000 },
	{ "equip_open2", function() db.actor:move_to_slot(glowstick(), 9) end, 1500 },
	{ "det_in_window", function() press(det_key) end, 3000 },
	{ "inv_key3", inv_key, 4000 },
	{ "det_after", function() press(det_key) end, 3000 },
	{ "slot3_after", function() db.actor:activate_slot(3) end, 2500 },
	{ "inv_key4", inv_key, 600 },
	{ "det_during_show", function() press(det_key) end, 4000 },
	{ "inv_key5", inv_key, 3000 },
	{ "det_end", function() press(det_key) end, 3000 },
}
-- The user's case: the exoskeleton set (a Desert Eagle in the hand), a glowstick drawn beside it, an antirad eaten
-- through its scene, then the inventory key.
local function first_of(sec)
	local g
	db.actor:iterate_inventory(function(_, item) if item:section() == sec then g = item end end, nil)
	return g
end
plans.eat = {
	{ "set_heavy", function() dead_air_x64_debug_menu.act("loadouts", "heavy", "primary", 1, 2) end, 3000 },
	{ "glow_spawn", function()
		alife():create("device_glowstick", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
	end, 1500 },
	{ "glow_slot", function() db.actor:move_to_slot(glowstick(), 9) end, 1000 },
	{ "deagle", function() db.actor:activate_slot(5) end, 2500 },
	{ "glow_key", function() press(det_key) end, 3000 },
	{ "eat_antirad", function() db.actor:eat(first_of("antirad")) end, 1500 },
	{ "eat_mid", function() end, 2500 },
	{ "eat_after", function() end, 4000 },
	{ "inv_key", inv_key, 2000 },
	{ "inv_key_wait", function() end, 4000 },
	{ "inv_key2", inv_key, 3000 },
	{ "inv_key3", inv_key, 5000 },
}
-- The antirad taken from the open inventory (the backpack scene holds the hands), then the window closed and opened.
plans.eat_inv = {
	{ "set_heavy", function() dead_air_x64_debug_menu.act("loadouts", "heavy", "primary", 1, 2) end, 3000 },
	{ "glow_spawn", function()
		alife():create("device_glowstick", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
	end, 1500 },
	{ "glow_slot", function() db.actor:move_to_slot(glowstick(), 9) end, 1000 },
	{ "deagle", function() db.actor:activate_slot(5) end, 2500 },
	{ "glow_key", function() press(det_key) end, 3000 },
	{ "inv_open", inv_key, 4000 },
	{ "eat_in_window", function() db.actor:eat(first_of("antirad")) end, 1500 },
	{ "eat_mid", function() end, 3000 },
	{ "eat_after", function() end, 4000 },
	{ "inv_key", inv_key, 2000 },
	{ "inv_key_wait", function() end, 5000 },
	{ "inv_key2", inv_key, 3000 },
	{ "inv_key3", inv_key, 6000 },
	{ "inv_key4", inv_key, 3000 },
}
-- Aiming a one-handed pistol with the glowstick out: the glowstick stays in the left hand.
plans.aim = {
	{ "set_heavy", function() dead_air_x64_debug_menu.act("loadouts", "heavy", "primary", 1, 2) end, 3000 },
	{ "glow_spawn", function()
		alife():create("device_glowstick", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
	end, 1500 },
	{ "glow_slot", function() db.actor:move_to_slot(glowstick(), 9) end, 1000 },
	{ "deagle", function() db.actor:activate_slot(5) end, 2500 },
	{ "glow_key", function() press(det_key) end, 3000 },
	{ "aim", function() level.press_action(level.action_id("wpn_zoom")) end, 2000 },
	{ "aim_hold", function() end, 1500 },
	{ "aim_release", function() level.release_action(level.action_id("wpn_zoom")) end, 2000 },
}
local plan

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	local function go(s) stage, t0 = s, now end

	if stage == "start" then
		cmd("g_god on")
		det_key = level.action_id("show_detector")
		plan = plans[qa_glow_plan or "keys"]
		local slots = {}
		for i = 0, 13 do
			local it = db.actor:item_in_slot(i)
			if it then slots[#slots + 1] = i .. "=" .. it:section() end
		end
		say("[qa] glowd level " .. level.name() .. " key " .. tostring(det_key) .. " slots " .. table.concat(slots, " "))
		hands("load")
		go("next")
	elseif stage == "next" and now - t0 > 500 then
		step = step + 1
		local s = plan[step]
		if not s then
			go("done")
			return
		end
		s[2]()
		go("settle")
	elseif stage == "settle" and now - t0 > plan[step][3] then
		shots = shots + 1
		hands(string.format("%02d %s", shots, plan[step][1]))
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		if now - t0 > 1100 then go("next") end
	elseif stage == "done" then
		say("[qa] glowd DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
