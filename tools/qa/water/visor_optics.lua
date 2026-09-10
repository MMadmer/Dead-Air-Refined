-- The visor's optics, on a lens big enough to read.
--
-- A bead is nine pixels wide on the rig's screen and nothing about how it bends the world can be
-- judged on it. qa_visor_blob holds a cap of water at the centre of the glass, so one screenshot
-- shows what a drop does to the horizon: the sky has to be at its BOTTOM and the ground at its
-- top, with a thin dark line round it, or the sign of the normal is wrong again.
local stage, t0, frames = 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		level.set_weather(qa_water_weather or "af3_bright_rain", true)
		cmd("qa_visor_wet 1")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 15000 then
		cmd("qa_visor_blob 7")
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 2500 then
		cmd("screenshot da_vis_lens")
		cmd("r__visor_drops_stats")
		cmd("qa_visor_state")
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 2000 then
		cmd("qa_visor_blob 0")
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 2500 then
		cmd("screenshot da_vis_field")
		t0 = now stage = 5
	elseif stage == 5 and now - t0 > 2500 then
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 6
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
