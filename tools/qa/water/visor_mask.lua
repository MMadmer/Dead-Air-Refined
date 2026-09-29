-- The visor's water belongs to a mask. With no mask HUD up - no mask on, or the mask HUD switched
-- off in the options - nothing may land on the glass however hard it rains, and a wet mask taken
-- off takes its drops with it. The stand wears no mask, so this puts a respirator on and takes it
-- off again in a storm, and at every step reads back what the engine was told (qa_visor_state:
-- worn, glass) and what the field holds (r__visor_drops_stats: how much of the glass is wet).
--
--   off     option off, no mask       glass no, nothing on the glass
--   on      option on, respirator on  glass yes, the drops have arrived
--   ruck    every visor in the ruck   glass no, the field cleared at once
--   again   respirator on again       glass yes, starting from dry glass
--   wet     eight seconds later       glass yes, wetter than "again"
--   switch  option off, mask still on glass no, cleared again
--
-- "Every visor": the respirator, and the suit too when it carries a visor of its own - the
-- stand's does, and with it on the mask HUD never came down.
local stage, t0, frames = 0, 0, 0
local helm = "helm_respirator" -- a mask the base mask HUD knows (dinamic_hud.helmets)
local opt_before = true
local function cmd(s) get_console():execute(s) end

local function set_option(v)
	if axr_main and axr_main.config then
		axr_main.config:w_value("mm_options", "enable_mask_hud", v)
	end
end

local function report(tag)
	local ok, err = pcall(function()
		printf(string.format("DA_MASK %s t %d hud %s exposure %s", tag, time_global(),
			tostring(dinamic_hud and dinamic_hud.ishud()), tostring(level.get_rain_exposure())))
	end)
	if not ok then printf("DA_MASK %s ERROR %s", tag, tostring(err)) end
	cmd("qa_visor_state")
	cmd("r__visor_drops_stats")
	cmd("flush")
end

local function put_respirator_on()
	local obj = db.actor:object(helm)
	if obj then
		db.actor:move_to_slot(obj, 12)
	else
		printf("DA_MASK no respirator in the inventory")
	end
end

local function take_visors_off()
	local visors = dinamic_hud and dinamic_hud.helmets or {}
	local helmet, outfit = db.actor:item_in_slot(12), db.actor:get_current_outfit()
	if helmet and visors[helmet:section()] then db.actor:move_to_ruck(helmet) end
	if outfit and visors[outfit:section()] then db.actor:move_to_ruck(outfit) end
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		level.set_weather(qa_water_weather or "af3_bright_storm", true)
		if axr_main and axr_main.config then
			opt_before = axr_main.config:r_value("mm_options", "enable_mask_hud", 1, false)
		end
		set_option(false)
		alife():create(helm, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(),
			db.actor:id())
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 10000 then
		report("off")
		set_option(true)
		put_respirator_on()
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 20000 then
		report("on")
		take_visors_off()
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 1500 then
		report("ruck")
		put_respirator_on()
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 1500 then
		report("again")
		t0 = now stage = 5
	elseif stage == 5 and now - t0 > 8000 then
		report("wet")
		set_option(false)
		t0 = now stage = 6
	elseif stage == 6 and now - t0 > 1500 then
		report("switch")
		set_option(opt_before)
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 7
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
