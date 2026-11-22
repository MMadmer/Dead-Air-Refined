-- The seams of looped hands motions (LoopSeam.h): a rifle drawn, the player sprints in a straight
-- line and the hands' lead_gun bone is logged every frame with the cycle's time, so the turn of
-- the loop can be read off the trace. Run once as it is and once with -anim_raw_loops; the log of
-- the first also names every motion the loader changed (-anim_loop_log).
--
--   tools\qa\Run-WaterQa.ps1 -Name loop-seam -Scenario loop_seam -Save qa_meadow -KeepShaderCache
--        -ExtraArgs "-anim_loop_log"
local stage, t0, frames, traced = 0, 0, 0, 0
local function cmd(s) get_console():execute(s) end
local function act(name) return level.action_id(name) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	device():pause(false)
	local now = time_global()
	local function after(ms) return now - t0 > ms end

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
		level.press_action(act("forward")) level.press_action(act("sprint_toggle"))
		t0 = now stage = 3
	elseif stage == 3 then
		level.hold_action(act("forward"))
		if after(1500) then
			cmd("qa_hands_bone lead_gun")
			traced = traced + 1
			if traced >= 240 then
				level.release_action(act("forward"))
				printf("DA_WATER_PROBE_DONE") cmd("flush")
				stage = 4
			end
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
