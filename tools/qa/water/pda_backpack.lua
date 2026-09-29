-- The 3D PDA and the backpack scene on the same hands (the PDA takes a rig of its own, the backpack
-- needs the ordinary one):
--   1. the PDA up, then the inventory key at once - the device goes away first, then the bag comes
--      out on the right hands; nothing hangs in front of the camera;
--   2. the PDA opens again afterwards;
--   3. the PDA key with the backpack held - the inventory closes, the bag is put away, then the
--      device comes up.
--
--   tools\qa\Run-WaterQa.ps1 -Name pda -Scenario pda_backpack -Save <save> -KeepShaderCache
--        (window frames: tools\qa\Watch-QaShots.ps1)
local stage, t0, frames = 0, 0, 0
local passed, total = 0, 0
local shots = 0

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

-- The PDA key, as CUIGameCustom::ShowPdaMenu takes it: the inventory goes, the window toggles.
local function pda_key()
	local pda = ActorMenu.get_pda_menu()
	if not (pda and pda:IsShown()) then get_hud():HideActorMenu() end
	if pda then pda:ShowDialog(true) end
end

local function menu_open()
	local am = ActorMenu.get_actor_menu()
	return am ~= nil and am:IsShown()
end

local function pda_up()
	return db.actor:active_slot() == 13
end

local function state_line()
	return string.format("slot %d, scene #%d active %s, menu %s", db.actor:active_slot(), game.hud_scene_id(),
		tostring(game.scene_active()), tostring(menu_open()))
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		pda_key()
		t0 = now stage = 1
	elseif stage == 1 and after(3000) then
		check("pda-raised", pda_up(), state_line())
		shot()
		-- The inventory key with the PDA in the hands.
		inventory_key()
		t0 = now stage = 2
	elseif stage == 2 and after(900) then
		printf("[qa] +0.9 s " .. state_line())
		shot()
		t0 = now stage = 3
	elseif stage == 3 and after(1300) then
		printf("[qa] +2.2 s " .. state_line())
		shot()
		t0 = now stage = 4
	elseif stage == 4 and after(2800) then
		cmd("qa_hands_state")
		check("backpack-after-pda", menu_open() and game.hud_scene_id() ~= 0 and not pda_up(), state_line())
		shot()
		inventory_key()
		t0 = now stage = 5
	elseif stage == 5 and after(2500) then
		cmd("qa_hands_state")
		check("closed-clean", not menu_open() and game.hud_scene_id() == 0, state_line())
		pda_key()
		t0 = now stage = 6
	elseif stage == 6 and after(3000) then
		check("pda-opens-again", pda_up(), state_line())
		shot()
		get_hud():HidePdaMenu()
		t0 = now stage = 7
	elseif stage == 7 and after(3000) then
		-- The backpack held, then the PDA key.
		inventory_key()
		t0 = now stage = 8
	elseif stage == 8 and after(3500) then
		check("backpack-held", menu_open() and game.hud_scene_id() ~= 0, state_line())
		pda_key()
		t0 = now stage = 9
	elseif stage == 9 and after(600) then
		printf("[qa] pda key +0.6 s " .. state_line())
		shot()
		t0 = now stage = 10
	elseif stage == 10 and after(3000) then
		cmd("qa_hands_state")
		check("pda-over-backpack", pda_up() and not menu_open() and game.hud_scene_id() == 0, state_line())
		shot()
		get_hud():HidePdaMenu()
		t0 = now stage = 11
	elseif stage == 11 and after(2500) then
		cmd("qa_hands_state")
		check("all-clean", not pda_up() and game.hud_scene_id() == 0 and not game.only_movekeys_allowed(), state_line())
		printf(string.format("[qa] DONE %d/%d", passed, total))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 12
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
