-- The visor, filmed - as the FIELD and not as the screen.
--
-- Everything this feature is about is motion: drops running, tracks being followed, a hand
-- smearing rather than deleting. A still frame shows none of it, and the rig cannot film the
-- screen either - a JPEG of every frame costs half a megabyte and drops it to about one frame a
-- second, which is slower than the drops. The field itself is one channel of a small target, so
-- r__visor_drops_stats dumps it as a pair of pictures for a few hundred microseconds: thickness,
-- and the film left behind. Five a second across the wipe is a strip that can be stepped through
-- and measured.
local stage, t0, frames, ticks = 0, 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		level.set_weather(qa_water_weather or "af3_bright_rain", true)
		cmd("qa_visor_wet 1")
		-- A wind to lean the tracks over. The drops are pushed by the air the glass is moving
		-- through, so with none of it they run dead vertically and the slant cannot be checked
		-- at all; a rain cycle's own wind is whatever the weather felt like that hour.
		cmd("wind_force 0.35")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 20000 then
		-- Twenty seconds of rain first: the point is to film water that has had time to merge
		-- and get over the sliding threshold, not the first arrivals.
		cmd("qa_visor_state")
		printf(string.format("DA_WATER_T %d roll", time_global()))
		t0 = now stage = 2
	elseif stage == 2 then
		-- Free running: four seconds of the field on its own.
		if now - ticks > 200 then
			cmd("r__visor_drops_stats")
			ticks = now
		end
		if now - t0 > 4000 then
			cmd("screenshot da_vis_run")
			printf(string.format("DA_WATER_T %d wipe", time_global()))
			cmd("visor_wipe")
			t0 = now stage = 3
		end
	elseif stage == 3 then
		-- The sweep and a moment after it.
		if now - ticks > 150 then
			cmd("r__visor_drops_stats")
			ticks = now
		end
		if now - t0 > 2500 then
			cmd("screenshot da_vis_wiped")
			printf(string.format("DA_WATER_T %d cut", time_global()))
			t0 = now stage = 4
		end
	elseif stage == 4 and now - t0 > 5000 then
		cmd("wind_force -1")
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 5
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
