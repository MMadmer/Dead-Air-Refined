-- Turns the actor at a steady rate and records frames, so the motion vectors and the temporal
-- resolvers are seen under camera motion (tools\qa\Run-WaterQa.ps1 -Scenario motion_check
-- -Globals "qa_render_cmds = { 'r__aa st_aa_taa', 'r__motion_vectors 1' }"). The header's
-- qa_render_cmds set the mode; a vid_restart among them is waited out before recording.
local c = get_console()
local stage, t0, frames, yaw, last = 0, 0, 0, 0, 0

local function cmd(s) c:execute(s) end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 150 then return end
	local now = time_global()

	if stage == 0 then
		for _, s in ipairs(qa_render_cmds or {}) do cmd(s) end
		yaw = db.actor:direction():getH()
		t0, stage = now, 1
	elseif stage == 1 and now - t0 > 4000 then
		cmd("r__capture 2 10")
		t0, last, stage = now, now, 2
	elseif stage == 2 then
		-- About 30 degrees a second, from the recording's first frame on.
		yaw = yaw + 0.52 * (now - last) / 1000
		last = now
		db.actor:set_actor_direction(-yaw)
		if now - t0 > 3000 then
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			stage = 3
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
