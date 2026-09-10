-- The visor driven the way the game drives it, watched for two minutes.
--
-- The stand wears no mask, so dead_air_x64_visor.script would push a rate of zero; with the mask
-- HUD option off it treats the drops as unconditional and pushes level.get_rain_exposure() the way
-- it does for a player. Nothing is pinned: no qa_visor_wet, no wind_force. Every two seconds the
-- rate the engine is holding, the exposure the script reads, and the field's own statistics -
-- including its NaN count - go to the log, and a wipe goes across at the minute. If the drops
-- stop arriving, this says which link let go: the exposure, the push, or the field.
local stage, t0, frames, ticks = 0, 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function report(tag)
	local ok, err = pcall(function()
		printf(string.format("DA_LIVE %s t %d factor %s exposure %s hud %s", tag, time_global(),
			tostring(level.rain_factor()), tostring(level.get_rain_exposure()),
			tostring(dinamic_hud and dinamic_hud.ishud())))
	end)
	if not ok then printf("DA_LIVE %s ERROR %s", tag, tostring(err)) end
	cmd("qa_visor_state")
	cmd("r__visor_drops_stats")
	cmd("flush")
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		level.set_weather(qa_water_weather or "af3_bright_storm", true)
		if axr_main and axr_main.config then
			axr_main.config:w_value("mm_options", "enable_mask_hud", false)
		end
		t0 = now ticks = now stage = 1
	elseif stage == 1 then
		if now - ticks > 2000 then
			report("run") ticks = now
		end
		if now - t0 > 60000 then
			cmd("visor_wipe 0 0.55")
			printf("DA_LIVE wipe")
			stage = 2
		end
	elseif stage == 2 then
		if now - ticks > 2000 then
			report("wiped") ticks = now
		end
		if now - t0 > 125000 then
			if axr_main and axr_main.config then
				axr_main.config:w_value("mm_options", "enable_mask_hud", true)
			end
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			stage = 3
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
