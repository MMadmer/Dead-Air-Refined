-- R5 ai-levels (docs\dead-air\ai\PLAN.md 7.3): two level changes. After each load the probe prints the AI counters
-- (ai_stats) next to the number of online stalkers it counts itself: the registries must hold this level's stalkers
-- only (nothing carried over from the level before), and the log must stay free of errors.
--
--   tools\qa\ai\Invoke-AiRun.ps1 -Name r5-ai-levels -Scenario ai_levels -Save foliage
--
-- The phase rides in the save's script data (a level change saves and loads the game).
local function cmd(s) get_console():execute(s) end

local t0
local function t_now() return t0 and (time_global() - t0) / 1000 or 0 end

local function say(fmt, ...)
	printf("%s", string.format("[qa-ai] t=%.2f ", t_now()) .. string.format(fmt, ...))
end

-- There and back again, from Cordon.
local ROUTE = {"k00_marsh", "l01_escape"}

local phase, reported, jumped, finished, quit_at

local function data()
	return alife_storage_manager.get_state()
end

local function online_stalkers()
	local count = 0
	for id = 1, 65534 do
		local obj = level.object_by_id(id)
		if obj and IsStalker(obj) and obj:alive() then count = count + 1 end
	end
	return count
end

local function finish(verdict)
	if finished then return end
	finished = true
	data().qa_ai_levels = nil
	say("done %s", verdict)
	printf("%s", "DA_WATER_PROBE_DONE")
	cmd("flush")
	quit_at = time_global() + 3000
end

local function step()
	local t = t_now()
	if not reported and t >= 4 then
		reported = true
		say("level phase=%d name=%s online_stalkers=%d", phase, level.name(), online_stalkers())
		cmd("ai_stats")
	elseif reported and not jumped and t >= 6 then
		jumped = true
		if phase >= #ROUTE then
			finish("complete")
			return
		end
		data().qa_ai_levels = phase + 1
		say("jump phase=%d to=%s", phase, ROUTE[phase + 1])
		cmd("flush")
		game.jump_to_level(ROUTE[phase + 1])
	end
end

local function on_first_update()
	cmd("g_pause_in_background 0")
	cmd("main_menu off")
	device():pause(false)
	cmd("g_god 1")
	cmd("ai_log_period 5000")
	t0 = time_global()
	phase = data().qa_ai_levels or 0
	say("start scenario=ai_levels phase=%d level=%s", phase, level.name())
end

local function on_update()
	device():pause(false)
	if not t0 then return end
	if quit_at then
		if time_global() >= quit_at then
			quit_at = nil
			cmd("quit")
		end
		return
	end
	if finished then return end
	local ok, err = pcall(step)
	if not ok then
		say("error lua=%s", tostring(err))
		finish("error")
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_first_update", on_first_update)
	RegisterScriptCallback("actor_on_update", on_update)
end
