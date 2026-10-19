-- The blades' reach (docs/dead-air/GRASS.md, "Reach"): the thinning reach against the stock one in one session,
-- at the eye and from above. For each view, "new" (the thinning, kReach x the detail radius) then "old"
-- (r__dbg_grass 256: the detail radius at full cover), each announced as "[qa] reach shot <n> <view> <mode>"
-- and followed by r__grass_blades_stats (records per bin: the budgets are compared there).
--   qa_reach_up = 35     how far above the eye the high view is (m)
--   qa_reach_pitch = -0.2 its pitch (rad, down)
-- Game time stops and the HUD weapon is hidden on the first frame.
local c = get_console()
local stage, t0, frames, step, eye, dir0

local up = qa_reach_up or 35
local pitch = qa_reach_pitch or -0.2
local shots = { { "eye", "new" }, { "eye", "old" }, { "high", "new" }, { "high", "old" } }

local function cmd(s) c:execute(s) end

local function place(view)
	if view == "eye" then
		level.remove_cam_custom_position_direction()
		return
	end
	local pos = vector():set(eye.x, eye.y + up, eye.z)
	level.set_cam_custom_position_direction(pos, vector():set(dir0:getH(), pitch, 0))
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames == 1 then
		level.set_time_factor(0.0001)
		cmd("hud_weapon 0")
	end
	if frames < 120 then return end
	local now = time_global()
	local function go(next_stage) stage, t0 = next_stage, now end

	if stage == "wait" then
		eye = vector():set(device().cam_pos)
		dir0 = vector():set(device().cam_dir)
		printf("[qa] reach level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " preset " ..
			c:get_token("_preset") .. " density " .. c:get_token("r__grass_density"))
		step = 1
		go("set")
	elseif stage == "set" then
		local s = shots[step]
		if not s then
			go("done")
			return
		end
		place(s[1])
		cmd("r__dbg_grass " .. (s[2] == "old" and "256" or "0"))
		go("settle")
	elseif stage == "settle" and now - t0 > 3000 then
		local s = shots[step]
		printf("[qa] reach shot " .. step .. " " .. s[1] .. " " .. s[2])
		cmd("r__grass_blades_stats")
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		step = step + 1
		-- The screenshot's file name has a one-second resolution.
		if now - t0 > 0 then go("gap") end
	elseif stage == "gap" and now - t0 > 1200 then
		go("set")
	elseif stage == "done" then
		level.remove_cam_custom_position_direction()
		cmd("r__dbg_grass 0")
		cmd("hud_weapon 1")
		printf("[qa] reach DONE")
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = "end"
	end
end

function on_game_start()
	stage, t0, frames, step = "wait", 0, 0, 0
	RegisterScriptCallback("actor_on_update", on_update)
end
