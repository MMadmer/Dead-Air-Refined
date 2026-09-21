-- Profiles the game thread on a real save. The point is attribution: which of the engine's
-- stock functions actually burn time while the world is simulating, as opposed to which ones
-- look expensive when you read them.
--
-- The actor is left alone deliberately. What should be running is everything the save already
-- has running - A-Life, the NPCs in earshot, physics on whatever is settling, the renderer -
-- because that is the mix a player pays for.
local stage, t0, frames = 0, 0, 0

local function cmd(s) get_console():execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	-- Let the level settle before sampling: the first frames are still streaming.
	if frames < 180 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		printf("PROFILE: begin level=%s seconds=%s", tostring(level.name()),
			tostring(qa_profile_seconds or 20))
		cmd("flush")
		t0 = now stage = 1

	elseif stage == 1 and after(500) then
		cmd("dar_profile_start " .. tostring(qa_profile_hz or 1000))
		t0 = now stage = 2

	elseif stage == 2 and after((qa_profile_seconds or 20) * 1000) then
		cmd("dar_profile_stop " .. tostring(qa_profile_top or 45))
		cmd("flush")
		t0 = now stage = 3

	elseif stage == 3 and after(2000) then
		-- Ask who called the symbols the run was set up to investigate. Costs nothing: the
		-- samples are still in memory and this only reads them again.
		for _, needle in ipairs(qa_profile_callers or {}) do
			cmd("dar_profile_callers " .. needle)
		end
		cmd("flush")
		t0 = now stage = 4

	elseif stage == 4 and after(1500) then
		printf("PROFILE: DONE")
		cmd("flush")
		stage = 5
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
