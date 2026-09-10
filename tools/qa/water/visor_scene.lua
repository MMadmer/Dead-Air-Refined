-- The visor on a player's own save, watched for seventy-five seconds without touching anything.
--
-- A report says the drops dry up and come back in a storm where the player stands still. This
-- loads that save on the stand, leaves the weather the save restores unless the runner names one
-- ("keep" keeps it), and once a second writes what the wetting rate is built from: the weather
-- name, the rain density, the exposure the script reads, the cover under it, and the field's own
-- statistics. If the density holds while the exposure drops, the sky-cover probe is what let go.
local stage, t0, frames, ticks = 0, 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function report()
	local ok, err = pcall(function()
		local p = db.actor:position()
		printf(string.format("DA_LIVE run t %d factor %s exposure %s hud %s weather %s wfx %s at %s %.1f %.1f %.1f",
			time_global(), tostring(level.rain_factor()), tostring(level.get_rain_exposure()),
			tostring(dinamic_hud and dinamic_hud.ishud()), tostring(level.get_weather()),
			tostring(level.is_wfx_playing()), level.name(), p.x, p.y, p.z))
	end)
	if not ok then printf("DA_LIVE run ERROR %s", tostring(err)) end
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
		if qa_water_weather and qa_water_weather ~= "keep" then
			level.set_weather(qa_water_weather, true)
		end
		if axr_main and axr_main.config then
			axr_main.config:w_value("mm_options", "enable_mask_hud", false)
		end
		t0 = now ticks = now stage = 1
	elseif stage == 1 then
		if now - ticks > 1000 then
			report() ticks = now
			-- A picture of the spot every half minute, for the geometry the rays report.
			if math.floor((now - t0) / 1000) % 30 == 5 then cmd("screenshot") end
		end
		-- Seventy-five seconds: the report that motivated this closed within twenty and reopened
		-- within forty-five, so a run that shows nothing by here is a run that shows nothing.
		if now - t0 > 75000 then
			if axr_main and axr_main.config then
				axr_main.config:w_value("mm_options", "enable_mask_hud", true)
			end
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			stage = 2
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
