-- Player report of 1.6.3: a gas mask costs a lot of frame rate, and taking it off gives it back. One session, the same
-- spot and camera: no mask, mask on dry glass (the visor's steps must cost nothing), the glass wetted (they must run and
-- the water arrive), and the mask off. "[qa] mask <label>" brackets the [gpu] lines of each step; the first one of a step
-- is dropped when reading, it still averages the step before.
--
--   tools\qa\Run-WaterQa.ps1 -Name mask164 -Scenario mask_164 -Save foliage -KeepShaderCache -Seconds 120 -ExtraArgs "-r5"
local stage, t0, frames, step = "start", 0, 0, 0
local helm = "helm_respirator"
local clear = "af3_slight_clear_steady_3"

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function first_of(sec)
	local found
	db.actor:iterate_inventory(function(_, item) if item:section() == sec then found = item end end, nil)
	return found
end

local function mask_on() db.actor:move_to_slot(first_of(helm), 12) end
local function mask_off()
	local worn = db.actor:item_in_slot(12)
	if worn then db.actor:move_to_ruck(worn) end
end

local plan = {
	{ "clear_off", function() mask_off() end },
	{ "clear_on", function() mask_on() end },
	{ "clear_off2", function() mask_off() end },
	{ "dry_on", function() mask_on() end },
	{ "wet_on", function() cmd("qa_visor_wet 0.6") end },
	{ "wet_off", function() cmd("qa_visor_wet -1"); mask_off() end },
}

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()

	if stage == "start" then
		cmd("g_god on")
		level.set_time_factor(0.0001)
		level.set_weather(clear, true)
		if axr_main and axr_main.config then
			axr_main.config:w_value("mm_options", "enable_mask_hud", true)
		end
		local outfit = db.actor:get_current_outfit()
		if outfit then db.actor:move_to_ruck(outfit) end
		alife():create(helm, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		level.set_cam_custom_position_direction(device().cam_pos, vector():set(device().cam_dir:getH(), 0, 0), 0, false, false)
		cmd("r__gpu_log 60")
		stage, t0 = "next", now
	elseif stage == "next" and now - t0 > 3000 then
		step = step + 1
		local s = plan[step]
		if not s then
			cmd("r__gpu_log 0")
			level.remove_cam_custom_position_direction()
			say("DA_WATER_PROBE_DONE")
			stage = "end"
			return
		end
		s[2]()
		stage, t0 = "settle", now
	elseif stage == "settle" and now - t0 > 3000 then
		say(string.format("[qa] mask %s begin hud %s", plan[step][1], tostring(dinamic_hud and dinamic_hud.ishud())))
		cmd("qa_visor_state")
		stage, t0 = "measure", now
	elseif stage == "measure" and now - t0 > 9000 then
		say(string.format("[qa] mask %s end fps %.1f", plan[step][1], 1000.0 / math.max(device().time_delta or 1, 1)))
		cmd("qa_visor_state")
		cmd("r__visor_drops_stats")
		stage, t0 = "next", now
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
