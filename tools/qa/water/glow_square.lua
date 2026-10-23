-- The dark square in the light of a hand-held glow stick (user's save quicksave8 -> glowsq on the rig): the save's
-- own view, weather and time untouched. Each step of qa_plan ("base" or console commands split by "|") settles and
-- takes a shot, announced as "[qa] glow shot <n> <step>"; the steps' commands stay in force for the ones after.
--   qa_plan = "base;glow"   ("glow" puts a glowstick of the inventory into the detector slot and draws it)
--
--   tools\qa\Run-WaterQa.ps1 -Name glow-sq -Scenario glow_square -Save glowsq -KeepShaderCache -Seconds 60
local c = get_console()
local stage, t0, frames = "start", 0, 0
local plan, step, shots = {}, 0, 0
local glow = false

local function cmd(s) c:execute(s) end
local function say(s) printf(s) cmd("flush") end

local function state(tag)
	local a = db.actor:active_item()
	local det = db.actor:active_detector()
	local s9 = db.actor:item_in_slot(9)
	local n = 0
	db.actor:iterate_inventory(function(_, item) if item:section() == "device_glowstick" then n = n + 1 end end, nil)
	say(string.format("[qa] glow state %s active %s detector %s slot %d slot9 %s glowsticks %d", tag,
		a and a:section() or "-", det and det:section() or "-", db.actor:active_slot(), s9 and s9:section() or "-", n))
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	local function go(s) stage, t0 = s, now end
	if glow and frames % 30 == 0 and not db.actor:active_detector() then
		db.actor:restore_detector(true)
	end

	if stage == "start" then
		cmd("g_god on")
		for item in string.gmatch(qa_plan or "base", "[^;]+") do
			plan[#plan + 1] = item
		end
		say("[qa] glow level " .. level.name())
		state("load")
		go("next")
	elseif stage == "next" then
		step = step + 1
		local s = plan[step]
		if not s then
			go("done")
			return
		end
		if s == "glow" then
			-- The detector slot's glowstick, drawn as the hotkey would (retried in on_update until it is up).
			db.actor:iterate_inventory(function(_, item)
				if item:section() == "device_glowstick" and not glow then db.actor:move_to_slot(item, 9); glow = true end
			end, nil)
		elseif s ~= "base" then
			for part in string.gmatch(s, "[^|]+") do cmd(part) end
		end
		go("settle")
	elseif stage == "settle" and now - t0 > 2500 then
		shots = shots + 1
		say(string.format("[qa] glow shot %02d %s", shots, plan[step]))
		state("shot")
		cmd("r__screenshot_every 1")
		go("shot")
	elseif stage == "shot" then
		cmd("r__screenshot_every 0")
		if now - t0 > 1100 then go("next") end
	elseif stage == "done" then
		say("[qa] glow DONE")
		say("DA_WATER_PROBE_DONE")
		stage = "end"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
