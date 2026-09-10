-- What the visor is told the rain is, by day and by night, side by side.
--
-- level.get_rain_volume() is a sound level with the old lighting hemi folded in: it dies at night
-- on purpose, because shipped scripts turn it into radiation and campfire dousing. Read as the
-- wetting rate it dried the visor in every night storm. level.get_rain_exposure() is density
-- times sky cover and nothing else. Both are printed in the same rain at noon and then twelve
-- hours later; the second must hold while the first falls away.
local stage, t0, frames, ticks, n = 0, 0, 0, 0, 0
local function cmd(s) get_console():execute(s) end

local function say(tag)
	-- Each reading flushed as it is made: the log is buffered, and a probe that dies before its
	-- final flush leaves nothing behind but the loading lines.
	local ok, err = pcall(function()
		printf(string.format("DA_RAIN_PROBE %s factor %s volume %s exposure %s", tag,
			tostring(level.rain_factor()), tostring(level.get_rain_volume()),
			tostring(level.get_rain_exposure and level.get_rain_exposure() or "no export")))
	end)
	if not ok then printf("DA_RAIN_PROBE %s ERROR %s", tag, tostring(err)) end
	cmd("flush")
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		level.set_weather(qa_water_weather or "af3_bright_rain", true)
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 8000 then
		ticks = now n = 0 stage = 2
	elseif stage == 2 then
		if now - ticks > 1000 then
			say("day") ticks = now n = n + 1
			if n >= 5 then
				local ok, err = pcall(function() level.change_game_time(0, 12, 0) end)
				if not ok then printf("DA_RAIN_PROBE time ERROR %s", tostring(err)) cmd("flush") end
				t0 = now stage = 3
			end
		end
	elseif stage == 3 and now - t0 > 8000 then
		ticks = now n = 0 stage = 4
	elseif stage == 4 then
		if now - ticks > 1000 then
			say("night") ticks = now n = n + 1
			if n >= 5 then
				printf("DA_WATER_PROBE_DONE")
				cmd("flush")
				stage = 5
			end
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
