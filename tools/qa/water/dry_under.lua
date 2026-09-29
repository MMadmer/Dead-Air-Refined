-- Dry rooms against real water, in one run (tools\qa\Run-WaterQa.ps1 -Scenario dry_under -Save qa_pedro).
--
-- Cordon, the rookie village: a cellar and a ground-floor room whose walls carry the liquid game
-- material. The field bake stamped such a wall at its top vertex, so the camera went "under water"
-- and the actor waded there on dry floor. Both spots must read dry. qa_water_dry_under also lists
-- any floor below a sheet with something solid in between (the column test's case) near the village.
--
-- Then the marsh (fogqa) is loaded in the same process and the camera dives into real water, which
-- must still read under water: the fix may not cost the lakes their underwater pass.
local frames, stage, t0, level_seen = 0, 0, 0, nil
local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

-- Feet on the floor of each spot (level data: floor height at that XZ).
local spots = {
	{ -238.9, -24.75, -134.5, "cellar" },
	{ -221.7, -19.90, -165.0, "room" },
}

local function report(tag)
	local p = device().cam_pos
	say(string.format("[qa] %s: camera %.2f %.2f %.2f", tag, p.x, p.y, p.z))
	cmd("qa_water_state")
	cmd("screenshot da_dry_" .. tag)
end

local function escape(now)
	if stage == 0 then
		cmd("qa_water_dry_under 80 0 0 -220 -150")
		stage, t0 = 1, now
	elseif stage <= #spots * 2 then
		local i = math.floor((stage + 1) / 2)
		local s = spots[i]
		if stage % 2 == 1 then
			db.actor:set_actor_position(vector():set(s[1], s[2], s[3]))
			stage, t0 = stage + 1, now
		elseif now - t0 > 2500 then
			report(s[4])
			stage, t0 = stage + 1, now
		end
	elseif now - t0 > 1500 then
		say("[qa] loading the marsh")
		cmd("load " .. (qa_dry_marsh_save or "fogqa"))
		stage = 100
	end
end

local function marsh(now)
	if stage == 0 then
		if qa_water_weather then level.set_weather(qa_water_weather, true) end
		cmd("qa_water_goto -0.12")
		stage, t0 = 1, now
	elseif stage == 1 and now - t0 > 3000 then
		report("marsh_above")
		cmd("qa_water_dive 0.8")
		stage, t0 = 2, now
	elseif stage == 2 and now - t0 > 2500 then
		report("marsh_under")
		stage, t0 = 3, now
	elseif stage == 3 and now - t0 > 800 then
		say("DA_WATER_PROBE_DONE")
		stage = 4
	end
end

local last_frame
local function on_update()
	if not db.actor then return end
	-- Registered once per game start; should the state survive the load, the second
	-- registration must not run every stage twice a frame.
	local f = device().frame
	if f == last_frame then return end
	last_frame = f
	local name = level.name()
	-- A level load may or may not rebuild this script's state: restart the stages on a new level.
	if name ~= level_seen then
		level_seen, frames, stage = name, 0, 0
	end
	frames = frames + 1
	if frames < 60 then return end
	local now = time_global()
	if name == "l01_escape" then
		escape(now)
	elseif name == "k00_marsh" then
		marsh(now)
	elseif stage == 0 then
		say("[qa] unexpected level " .. tostring(name))
		say("DA_WATER_PROBE_DONE")
		stage = 1
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
