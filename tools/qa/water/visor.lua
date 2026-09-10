-- The visor in the rain: drops arriving, running, and what a wipe leaves behind. The frames are
-- what the player actually looks through, so they are taken from wherever the actor stands - no
-- teleport - with the weather pinned to a downpour and the drop field read back beside each.
local stage, t0, frames = 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function shot(name)
	printf(string.format("DA_WATER_T %d %s", time_global(), name))
	cmd("screenshot " .. name)
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		level.set_weather(qa_water_weather or "af3_bright_storm", true)
		-- The stand wears no mask, so nothing drives the effect there: pin the rate by hand.
		cmd("qa_visor_wet 1")
		cmd("qa_visor_state")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 4000 then
		shot("da_vis_0_early")             -- the first arrivals, still small and pinned
		cmd("r__visor_drops_stats")
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 12000 then
		shot("da_vis_1_wet")               -- a quarter of a minute in: sliding drops and trails
		cmd("r__visor_drops_stats")
		cmd("qa_visor_state")
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 500 then
		cmd("visor_wipe")               -- the hand goes across
		printf(string.format("DA_WATER_T %d wipe", time_global()))
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 400 then
		shot("da_vis_2_wiping")            -- mid-sweep: cleared behind the hand, piled ahead of it
		t0 = now stage = 5
	elseif stage == 5 and now - t0 > 900 then
		shot("da_vis_3_wiped")             -- what the wipe left: streaks, not glass
		cmd("r__visor_drops_stats")
		t0 = now stage = 6
	elseif stage == 6 and now - t0 > 4000 then
		shot("da_vis_4_rewet")             -- the rain taking it back
		cmd("r__visor_drops_stats")
		t0 = now stage = 7
	elseif stage == 7 and now - t0 > 5000 then
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 8
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
