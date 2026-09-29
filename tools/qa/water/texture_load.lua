-- Texture loading numbers (docs/dead-air/dx12-features/dstorage.md 3.7, 9 runs 3 and 7). The level load logs
-- its "[texload] batch" and "[texload] geometry" lines by itself; this waits for the level, prints the
-- synchronous loads so far (r__texload_stats), spawns a few stock items next to the player (their models and
-- textures load synchronously, like anything that comes online in play), lets the game run and prints the
-- numbers again.
--
--   tools\qa\dx12\Invoke-Dx12Stand.ps1 -Name ds-s8-texload-pedro -Scenario texture_load -ExtraArgs '-r5 -always_active'
--
-- qa_texload_ms: how long the game runs after the spawns (default 20 s, at least 5 s + qa_fps_ms); FPS is taken
-- over that window after its first 5 s ("[qa] FPS", the perf batch's column).
-- qa_level_change = true: then jumps to qa_level_to (default l02_garbage) and back, printing the numbers on each
-- level after qa_texload_level_ms (default 8 s) of play there. Each unload prints its synchronous loads, each
-- load its batch lines; "[texload] live textures" of the two visits of the first level is the leak check.
-- A level change reconnects and restarts the script engine: the phase lives in the actor's saved variables
-- (the level-change autosave carries them), this file's locals start over on every level.
local frames = 0
local t0 = 0
local arrived = nil
local fps_t0, fps_frames = nil, 0
local fallback = {}

-- Stock sections; whichever exist in this installation are spawned.
local ITEMS = { "wpn_ak74", "wpn_pm", "wpn_toz34", "wpn_mp5", "medkit", "bandage", "grenade_f1", "device_torch",
	"detector_simple", "conserva", "vodka", "bread" }

local function cmd(s) get_console():execute(s) end

local function load_state(key, default)
	if load_var then
		local ok, value = pcall(load_var, db.actor, key, default)
		if ok and value ~= nil then return value end
	end
	if fallback[key] == nil then return default end
	return fallback[key]
end

local function save_state(key, value)
	fallback[key] = value
	if save_var then pcall(save_var, db.actor, key, value) end
end

local function spawn_items()
	local ini = system_ini()
	local pos, lvid, gvid = db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id()
	local spawned = 0
	for _, section in ipairs(ITEMS) do
		if ini:section_exist(section) then
			alife():create(section, pos, lvid, gvid)
			spawned = spawned + 1
		end
	end
	printf(string.format("[qa] texload spawned %d stock items", spawned))
end

local function done()
	save_state("qa_texload_phase", 99)
	printf("DA_WATER_PROBE_DONE")
	cmd("flush")
end

local function jump(target)
	printf("[qa] texload level change %s -> %s", level.name(), target)
	cmd("flush")
	game.jump_to_level(target)
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	local phase = load_state("qa_texload_phase", 0)
	local level_ms = qa_texload_level_ms or 8000

	if phase == 0 then
		local c = get_console()
		printf("[qa] renderer %s", tostring(c:get_string("renderer")))
		printf("[qa] caps %s", tostring(c:get_string("r__caps")))
		printf("[qa] level %s", level.name())
		cmd("r__texload_stats")
		spawn_items()
		t0 = now
		save_state("qa_texload_phase", 1)
	elseif phase == 1 and now - t0 <= math.max(qa_texload_ms or 20000, 5000 + (qa_fps_ms or 12000)) then
		if now - t0 > 5000 then
			if fps_t0 then fps_frames = fps_frames + 1 else fps_t0, fps_frames = now, 0 end
		end
	elseif phase == 1 then
		if fps_t0 and now > fps_t0 then
			printf(string.format("[qa] FPS %.1f over %d frames", fps_frames * 1000 / (now - fps_t0), fps_frames))
		end
		cmd("r__texload_stats")
		if not qa_level_change then
			done()
			return
		end
		save_state("qa_texload_origin", level.name())
		save_state("qa_texload_phase", 2)
		jump(qa_level_to or "l02_garbage")
	elseif phase == 2 or phase == 3 then
		local origin = load_state("qa_texload_origin", "")
		-- Phase 2 plays on the neighbour, phase 3 on the first level again.
		if (phase == 2) == (level.name() == origin) then return end
		arrived = arrived or now
		if now - arrived < level_ms then return end
		printf("[qa] level %s%s", level.name(), phase == 3 and " (back)" or "")
		cmd("r__texload_stats")
		if phase == 2 then
			save_state("qa_texload_phase", 3)
			jump(origin)
		else
			done()
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
