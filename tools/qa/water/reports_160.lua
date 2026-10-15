-- Player reports of 1.5.0, fixed in 1.6.0, in one short run:
--   1. medicine: the cycle lengths against the scene lengths (printed; the engine no longer stretches);
--   2. the backpack with a glowstick in the left hand: the lamp goes away before the scene and comes back after;
--   3. a quick slot naming vodka_3 uses the vodka_2 the player carries;
--   4. an animation item stranded in the animation slot does not keep the 3D PDA down;
--   5. RMB in the PDA's cursor mode stays in the cursor mode (it is the pages' click now).
--
--   tools\qa\Run-WaterQa.ps1 -Name reports -Scenario reports_160 -Save fogqa -KeepShaderCache -Seconds 90
local stage, t0, frames = 0, 0, 0
local passed, total = 0, 0
local shots = 0
local shot_frame = -1

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	say("[qa] " .. step .. " " .. (ok and "PASS" or "FAIL") .. " " .. tostring(detail or ""))
end

local function shot()
	shots = shots + 1
	say(string.format("[qa] SHOT %02d", shots))
	cmd("r__screenshot_every 1")
	shot_frame = frames
end

local function give(sec)
	return alife():create(sec, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
end

local function count(sec)
	local n = 0
	db.actor:iterate_inventory(function(_, item) if item:section() == sec then n = n + 1 end end, nil)
	return n
end

local function inventory_key()
	local am = ActorMenu.get_actor_menu()
	if am and not am:IsShown() then
		if _G.da_before_inventory() then get_hud():ShowActorMenu() end
	else
		get_hud():ShowActorMenu()
	end
end

local function pda_key()
	local pda = ActorMenu.get_pda_menu()
	if not (pda and pda:IsShown()) then get_hud():HideActorMenu() end
	if pda then pda:ShowDialog(true) end
end

local function glow()
	return db.actor:item_in_slot(9)
end

local function det_state()
	local d = glow()
	return d and d:get_state() or -1
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		-- 1. medicine
		for _, sec in ipairs({ "medkit", "bandage", "drug_coagulant", "antirad", "vodka", "bio_bandage" }) do
			local ini = ini_file("items\\items\\anims_list.ltx")
			local anm = ini:r_string_ex(sec, "anm")
			local tm = ini:r_float_ex(sec, "tm") or 0
			local len = anm and game.get_motion_length(anm .. "_hud", "anm_ea_show", 1.0) or 0
			say(string.format("[qa] medicine %s: cycle %s ms, scene %s ms", sec, tostring(len), tostring(tm)))
		end
		give("device_glowstick")
		give("vodka_2")
		give("animation_use_left_hand")
		t0 = now stage = 1
	elseif stage == 1 and after(1500) then
		db.actor:iterate_inventory(function(_, item)
			if item:section() == "device_glowstick" then db.actor:move_to_slot(item, 9) end
		end, nil)
		t0 = now stage = 2
	elseif stage == 2 and after(800) then
		db.actor:restore_detector(false)
		t0 = now stage = 3
	elseif stage == 3 and after(2500) then
		-- 2. backpack with the lamp out
		check("glow-out", det_state() == 0, "state " .. det_state())
		shot()
		inventory_key()
		t0 = now stage = 4
	elseif stage == 4 and after(3500) then
		check("glow-put-away-for-bag", det_state() == 3 and game.scene_active(), "state " .. det_state() .. ", scene " .. tostring(game.scene_active()))
		shot()
		get_hud():HideActorMenu()
		t0 = now stage = 5
	elseif stage == 5 and after(4000) then
		check("glow-back-after-bag", det_state() == 0 and db.actor:active_detector() ~= nil, "state " .. det_state())
		shot()
		-- 3. quick slot by kind
		cmd("slot_0 vodka_3")
		t0 = now stage = 6
	elseif stage == 6 and after(500) then
		say("[qa] vodka_2 before: " .. count("vodka_2"))
		level.press_action(level.action_id("quick_use_1"))
		level.release_action(level.action_id("quick_use_1"))
		t0 = now stage = 7
	elseif stage == 7 and after(9000) then
		check("quick-slot-kind", count("vodka_2") == 0, "vodka_2 " .. count("vodka_2") .. ", vodka " .. count("vodka"))
		-- 4. the stranded animation item
		local anm = db.actor:item_in_slot(13)
		say("[qa] animation slot before the PDA: " .. tostring(anm and anm:section()) .. ", active " .. tostring(db.actor:active_slot()))
		pda_key()
		t0 = now stage = 8
	elseif stage == 8 and after(7000) then
		local anm = db.actor:item_in_slot(13)
		check("pda-past-stranded-item", db.actor:active_slot() == 13 and anm and anm:section() ~= "animation_use_left_hand",
			tostring(anm and anm:section()) .. ", active " .. tostring(db.actor:active_slot()))
		shot()
		-- 5. RMB: into the cursor mode, then RMB on the page
		level.press_action(level.action_id("wpn_zoom"))
		level.release_action(level.action_id("wpn_zoom"))
		t0 = now stage = 9
	elseif stage == 9 and after(2500) then
		cmd("qa_hands_state")
		cmd("dar_ui_click right")
		t0 = now stage = 10
	elseif stage == 10 and after(1500) then
		-- the log line says whether the cursor mode survived the right click
		cmd("qa_hands_state")
		shot()
		cmd("dar_ui_click left")
		t0 = now stage = 11
	elseif stage == 11 and after(1000) then
		say(string.format("[qa] %d/%d passed", passed, total))
		say("DA_WATER_PROBE_DONE")
		cmd("quit")
		stage = 12
	end
	if shot_frame >= 0 and frames > shot_frame then
		cmd("r__screenshot_every 0")
		shot_frame = -1
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
