-- The shader permutations a release ships compiled (tools\package\build_shader_pack.ps1): every quality preset with
-- each anti-aliasing mode, a renderer restart each, the camera turned round the eye so the scene, the sky and the
-- weathers' passes are drawn. "[qa] sweep <preset> <aa>" per step, "[qa] sweep DONE" at the end.
--   qa_sweep_presets = "Minimum,Low,Default,High,Extreme"
--   qa_sweep_aa = "st_aa_taa,st_aa_smaa,st_aa_fxaa,st_aa_off"
local c = get_console()
local stage, t0, frames = "start", 0, 0
local plan, step = {}, 0
local dir0

local function cmd(s) c:execute(s) end
local function say(s) printf(s) cmd("flush") end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	local function go(s) stage, t0 = s, now end

	if stage == "start" then
		cmd("g_god on")
		dir0 = vector():set(device().cam_dir)
		for p in string.gmatch(qa_sweep_presets or "Minimum,Low,Default,High,Extreme", "[^,]+") do
			for a in string.gmatch(qa_sweep_aa or "st_aa_taa,st_aa_smaa,st_aa_fxaa,st_aa_off", "[^,]+") do
				plan[#plan + 1] = { preset = p, aa = a }
			end
		end
		say("[qa] sweep renderer " .. c:get_string("renderer") .. ", " .. #plan .. " steps")
		go("next")
	elseif stage == "next" then
		step = step + 1
		local s = plan[step]
		if not s then
			level.remove_cam_custom_position_direction()
			say("[qa] sweep DONE")
			say("DA_WATER_PROBE_DONE")
			stage = "end"
			return
		end
		cmd("_preset " .. s.preset)
		cmd("r__aa " .. s.aa)
		cmd("vid_restart")
		go("turn")
	elseif stage == "turn" and now - t0 > 2500 then
		-- a full turn and a look up and down, a second each
		local k = math.min((now - t0 - 2500) / 4000, 1)
		local eye = device().cam_pos
		level.set_cam_custom_position_direction(eye, vector():set(dir0:getH() + k * 6.283, math.sin(k * 12.566) * 0.6, 0),
			0, false, false)
		if k >= 1 then
			say("[qa] sweep " .. plan[step].preset .. " " .. plan[step].aa)
			go("next")
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
