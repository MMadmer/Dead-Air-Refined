-- The pipeline prewarm (d12_prewarm.cpp, vk_prewarm.cpp): the level plays 15 s and the game quits, the log keeps
-- "pipeline prewarm: N pipelines queued, M compiled|built on demand so far" of the level load and the precache's
-- end, and each pass's "pipeline prewarm: N pipelines in X ms (...)". A run with an empty journal fills it; the
-- next one, without the driver's pipelines.bin, shows the prewarm compiling them and the on-demand count falling.
--
--   tools\qa\Run-WaterQa.ps1 -Name prewarm-r5 -Scenario pipeline_prewarm -Save qa_meadow -KeepShaderCache -ExtraArgs "-r5" -Seconds 60
local stage, t0 = "start", 0
local function say(s) printf(s) get_console():execute("flush") end

local function on_update()
	if not db.actor or stage == "end" then return end
	device():pause(false)
	if stage == "start" then
		stage, t0 = "play", time_global()
	elseif time_global() - t0 > 15000 then
		say("[qa] prewarm played 15 s")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
