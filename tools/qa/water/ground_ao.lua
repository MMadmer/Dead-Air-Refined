-- The ground's sky occlusion under the blade grass (docs/dead-air/GRASS.md, "Shading"): looking down at the ground
-- between the blades under an overcast sky, where only the hemi light shows how the stand shades its foot. For each
-- view, each mode of qa_gao_modes in turn (console lines run before the shot), announced as
-- "[qa] gao shot <n> <view> <mode>", after the GPU timers of the settle's second half (r__gpu_stats).
--   qa_gao_modes = { { "on", "" }, { "off", "r2_ssao_mode disabled" } }
-- Game time stops and the HUD weapon is hidden on the first frame.
local c = get_console()
local stage, t0, frames, step, eye, dir0

local modes = qa_gao_modes or { { "gtao", "r2_ssao_mode gtao" }, { "noao", "r2_ssao_mode disabled" } }
local views = { { "down", 1.6, -1.15, 0 }, { "slope", 1.6, -0.45, 0 }, { "side", 1.6, -0.8, 1.6 } }
local shots = {}
for _, v in ipairs(views) do
	for _, m in ipairs(modes) do shots[#shots + 1] = { v, m } end
end

local function cmd(s) c:execute(s) end

local function place(v)
	local pos = vector():set(eye.x, eye.y + v[2] - 1.7, eye.z)
	level.set_cam_custom_position_direction(pos, vector():set(dir0:getH() + v[4], v[3], 0))
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames == 1 then
		level.set_time_factor(0.0001)
		cmd("hud_weapon 0")
		if qa_water_weather then level.set_weather(qa_water_weather, true) end
	end
	if frames < 120 then return end
	local now = time_global()
	local function go(next_stage) stage, t0 = next_stage, now end

	if stage == "wait" then
		eye = vector():set(device().cam_pos)
		dir0 = vector():set(device().cam_dir)
		printf("[qa] gao level " .. level.name() .. " renderer " .. c:get_string("renderer") .. " preset " ..
			c:get_token("_preset") .. " weather " .. tostring(qa_water_weather))
		step = 1
		go("set")
	elseif stage == "set" then
		local s = shots[step]
		if not s then
			go("done")
			return
		end
		place(s[1])
		for line in string.gmatch(s[2][2], "[^;]+") do cmd(line) end
		go("settle")
	elseif stage == "settle" and now - t0 > 1500 and now - t0 <= 3000 then
		-- The cost: the GPU timers of every frame of the settle's second half ("* [gpu] ... scene=").
		cmd("r__gpu_stats")
	elseif stage == "settle" and now - t0 > 3000 then
		local s = shots[step]
		printf("[qa] gao shot " .. step .. " " .. s[1][1] .. " " .. s[2][1])
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
		cmd("hud_weapon 1")
		printf("[qa] gao DONE")
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = "end"
	end
end

function on_game_start()
	stage, t0, frames, step = "wait", 0, 0, 0
	RegisterScriptCallback("actor_on_update", on_update)
end
