-- Transitions between the hands' movement cycles: a rifle drawn, the player walks, strafes (the
-- same cycle must run on), crouches and stands while walking (moving <-> moving crouch), sprints
-- and stops. The hands' lead_gun bone is logged every frame with the cycle's time, and every
-- change is marked, so the jolt after each one can be read off the trace. Run once as it is and
-- once with -PreCommands 'hud_cycle_match 0' (cycles start at their first frame, as before).
--
--   tools\qa\Run-WaterQa.ps1 -Name hud-trans -Scenario hud_transitions -Save qa_meadow -KeepShaderCache
--        -PreCommands 'rs_always_active on'
local stage, t0, frames = 0, 0, 0
local held = {}
local function cmd(s) get_console():execute(s) end
local function act(name) return level.action_id(name) end
local function mark(name)
	printf("[qa] MARK %s", name)
end
local function hold(name, on) held[name] = on or nil end
local function tap(name)
	level.press_action(act(name))
	level.release_action(act(name))
end

-- what happens, after how long the step before it ran (ms)
local steps = {
	{ 2500, function() hold("forward", true) mark("walk") end },
	{ 2500, function() hold("left", true) mark("strafe") end },
	{ 1500, function() hold("left", false) mark("unstrafe") end },
	{ 1500, function() tap("crouch") mark("crouch") end },
	{ 1800, function() tap("crouch") mark("stand") end },
	{ 1800, function() tap("sprint_toggle") mark("sprint") end },
	{ 2200, function() tap("sprint_toggle") mark("unsprint") end },
	{ 2000, function() hold("forward", false) mark("stop") end },
	{ 2000, function() mark("end") end },
}

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	device():pause(false)
	local now = time_global()
	for name in pairs(held) do level.hold_action(act(name)) end

	if stage == 0 then
		cmd("g_god on")
		alife():create("wpn_ak74", db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 800 then
		local w = db.actor:object("wpn_ak74")
		if w then db.actor:move_to_slot(w, 3) end
		db.actor:activate_slot(3)
		t0 = now stage = 2
	elseif stage >= 2 and stage - 1 <= #steps then
		if stage > 2 then cmd("qa_hands_bone lead_gun") end
		local step = steps[stage - 1]
		if now - t0 > step[1] then
			step[2]()
			t0 = now stage = stage + 1
		end
	elseif stage - 1 == #steps + 1 then
		for name in pairs(held) do level.release_action(act(name)) end
		held = {}
		printf("DA_WATER_PROBE_DONE") cmd("flush")
		stage = stage + 1
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
