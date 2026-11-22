-- Many hand-cycle switches at many phases, for an A/B of hud_cycle_match: a rifle drawn, the player
-- walks and toggles the sprint eight times, then stops and goes six times; the hands' lead_gun
-- bone is logged every frame and every switch is marked (tools: trace_trans-style peak per switch).
--
--   tools\qa\Run-WaterQa.ps1 -Name hud-sw-on -Scenario hud_switches -Save qa_meadow -KeepShaderCache
--        -PreCommands 'rs_always_active on','hud_cycle_match 2'
local stage, t0, frames = 0, 0, 0
local held = {}
local function cmd(s) get_console():execute(s) end
local function act(name) return level.action_id(name) end
local function mark(name) printf("[qa] MARK %s", name) end
local function tap(name)
	level.press_action(act(name))
	level.release_action(act(name))
end

local steps = { { 2500, function() held.forward = true mark("walk") end } }
-- uneven gaps, so each switch lands at another phase of the cycle it leaves
for i = 1, 8 do
	steps[#steps + 1] = { 1700 + 130 * i, function() tap("sprint_toggle") mark("sprint") end }
	steps[#steps + 1] = { 1500 + 170 * i, function() tap("sprint_toggle") mark("unsprint") end }
end
for i = 1, 6 do
	steps[#steps + 1] = { 1400 + 150 * i, function() held.forward = nil mark("stop") end }
	steps[#steps + 1] = { 1300 + 190 * i, function() held.forward = true mark("go") end }
end
steps[#steps + 1] = { 1500, function() held.forward = nil mark("end") end }

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
		level.release_action(act("forward"))
		held = {}
		printf("DA_WATER_PROBE_DONE") cmd("flush")
		stage = stage + 1
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
