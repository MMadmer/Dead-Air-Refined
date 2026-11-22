-- The upscalers on the running renderer (docs/dead-air/DX12_RENDERER.md 3): native, AMD FSR Quality, Intel XeSS
-- Quality and Native AA, NVIDIA DLSS Quality where the card has it, AMD FSR Quality with FSR frame generation (twice:
-- the second restart remakes the generating swap chain in place), then native again (the chain goes). Each
-- config restarts the renderer, settles, takes a shot ("[qa] SHOT <config>", the run's NN.jpg in that order) and a
-- frame-rate window ("[qa] fps <config> <x>"); the log has each backend's ready line ([FSR], [XeSS], [FSR FG]).
--
--   tools\qa\Run-WaterQa.ps1 -Name up-dx12 -Scenario upscaler_tour -Save qa_meadow -KeepShaderCache -Seconds 300
--        -ExtraArgs '-r5 -always_active'
local c = get_console()
local function cmd(s) c:execute(s) end

local off = { "r__fsr st_fsr_off", "r__dlss st_dlss_off", "r__xess st_xess_off", "r__framegen st_fg_off" }
local configs = {
	{ name = "native", cmds = {} },
	{ name = "fsr_q", cmds = { "r__fsr st_fsr_quality" } },
	{ name = "xess_q", cmds = { "r__xess st_xess_quality" } },
	{ name = "xess_aa", cmds = { "r__xess st_xess_native" } },
	{ name = "dlss_q", cmds = { "r__dlss st_dlss_quality" } },
	{ name = "fsr_q_fg", cmds = { "r__fsr st_fsr_quality", "r__framegen st_fg_fsr" } },
	{ name = "fsr_q_fg_again", cmds = { "r__fsr st_fsr_quality", "r__framegen st_fg_fsr" } },
	{ name = "native_after_fg", cmds = {} },
}
-- qa_up_only = { "fsr_q_fg", ... }: only these configs, in this order.
if qa_up_only then
	local by_name, picked = {}, {}
	for _, cfg in ipairs(configs) do by_name[cfg.name] = cfg end
	for _, name in ipairs(qa_up_only) do
		if by_name[name] then picked[#picked + 1] = by_name[name] end
	end
	configs = picked
end

local ci, phase, t0, frames, fps_frames = 1, "start", 0, 0, 0

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	device():pause(false)
	if frames < 150 then return end
	local now = time_global()
	if phase == "start" then
		cmd("g_god on")
		printf("[qa] caps %s", c:get_string("r__caps"))
		phase, t0 = "config", now
	elseif phase == "config" then
		local cfg = configs[ci]
		if not cfg then
			for _, x in ipairs(off) do cmd(x) end
			printf("DA_WATER_PROBE_DONE")
			cmd("flush")
			phase = "done"
			return
		end
		for _, x in ipairs(off) do cmd(x) end
		for _, x in ipairs(cfg.cmds) do cmd(x) end
		printf(string.format("[qa] config %s: fsr %s dlss %s xess %s fg %s", cfg.name, c:get_token("r__fsr"),
			c:get_token("r__dlss"), c:get_token("r__xess"), c:get_token("r__framegen")))
		cmd("vid_restart")
		phase, t0 = "settle", now
	elseif phase == "settle" and now - t0 > 5000 then
		printf(string.format("[qa] SHOT %s", configs[ci].name))
		cmd("flush")
		cmd("r__screenshot_every 1")
		phase, t0 = "shot", now
	elseif phase == "shot" then
		cmd("r__screenshot_every 0")
		phase, t0, fps_frames = "measure", now, 0
	elseif phase == "measure" then
		fps_frames = fps_frames + 1
		if now - t0 > 5000 then
			printf(string.format("[qa] fps %s %.1f", configs[ci].name, fps_frames * 1000 / (now - t0)))
			cmd("flush")
			ci, phase, t0 = ci + 1, "config", now
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
