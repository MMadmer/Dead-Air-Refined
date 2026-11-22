-- The lean offset of a hud item (lean_hud_offset_pos/rot): with a rifle drawn, the item's place in
-- front of the eye at rest, leaning left (mirrored), leaning right and leaning while aiming (gone).
-- qa_hands_state prints the place; the numbers are what this probe is about, the frames a look.
--
--   tools\qa\Run-WaterQa.ps1 -Name lean-hud -Scenario lean_hud -Save qa_meadow -KeepShaderCache
--        -Overlay <a copy of the overlay whose wpn_ak74_hud has lean keys>
local stage, t0, frames = 0, 0, 0
local hold = nil
local function cmd(s) get_console():execute(s) end
local function shot(name) printf("[qa] SHOT lean_%s", name) cmd("qa_hands_state") cmd("flush") end
local function act(name) return level.action_id(name) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	device():pause(false)
	local now = time_global()
	local function after(ms) return now - t0 > ms end
	for _, a in ipairs(hold or {}) do level.hold_action(act(a)) end

	if stage == 0 then
		cmd("g_god on")
		alife():create("wpn_ak74", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		t0 = now stage = 1
	elseif stage == 1 and after(800) then
		local w = db.actor:object("wpn_ak74")
		if w then db.actor:move_to_slot(w, 3) end
		db.actor:activate_slot(3)
		t0 = now stage = 2
	elseif stage == 2 and after(2500) then
		printf("[qa] active %s", tostring(db.actor:active_item() and db.actor:active_item():section()))
		shot("rest")
		t0 = now stage = 3
	elseif stage == 3 and after(1500) then
		level.press_action(act("llookout")) hold = { "llookout" }
		t0 = now stage = 4
	elseif stage == 4 and after(1500) then
		shot("left")
		t0 = now stage = 5
	elseif stage == 5 and after(1500) then
		-- let go and stand straight before the other side
		hold = nil level.release_action(act("llookout"))
		t0 = now stage = 6
	elseif stage == 6 and after(1500) then
		level.press_action(act("rlookout")) hold = { "rlookout" }
		t0 = now stage = 7
	elseif stage == 7 and after(1500) then
		shot("right")
		level.press_action(act("wpn_zoom")) hold = { "rlookout", "wpn_zoom" }
		t0 = now stage = 8
	elseif stage == 8 and after(1500) then
		shot("right_aim")
		t0 = now stage = 9
	elseif stage == 9 and after(1500) then
		hold = nil level.release_action(act("rlookout")) level.release_action(act("wpn_zoom"))
		printf("DA_WATER_PROBE_DONE") cmd("flush")
		stage = 10
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
