-- Spent cases across a level change: a burst on the first save, a load of the marsh while the cases lie and ring,
-- then a burst there. The unload frees every case model and the bounce sounds; the marsh reads the config anew.
--
--   tools\qa\Run-WaterQa.ps1 -Name shellsreload -Scenario shells_reload -Save puddle -KeepShaderCache -Seconds 150
local stage, t0, frames = 0, 0, 0
local level_seen
local last_frame

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function act(name, on)
	if on then level.press_action(level.action_id(name)) else level.release_action(level.action_id(name)) end
end

-- A rifle in the hands and a burst; the next stage once it is over.
local function burst(now, after, first, next_stage)
	if stage == first then
		for _, sec in ipairs({ "wpn_ak74", "ammo_5.45x39_fmj" }) do
			alife():create(sec, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		end
		t0 = now stage = first + 1
	elseif stage == first + 1 and after(1500) then
		db.actor:iterate_inventory(function(_, item)
			if item:section() == "wpn_ak74" then db.actor:move_to_slot(item, 3) end
		end, nil)
		t0 = now stage = first + 2
	elseif stage == first + 2 and after(700) then
		db.actor:activate_slot(3)
		t0 = now stage = first + 3
	elseif stage == first + 3 and after(3000) then
		act("wpn_fire", true)
		t0 = now stage = first + 4
	elseif stage == first + 4 and after(500) then
		act("wpn_fire", false)
		t0 = now stage = next_stage
	end
end

local function on_update()
	if not db.actor then return end
	-- registered once per game start; a state that survives the load must not run its stages twice a frame
	local f = device().frame
	if f == last_frame then return end
	last_frame = f
	device():pause(false)
	local name = level.name()
	if name ~= level_seen then
		level_seen, frames, stage = name, 0, 0
		say("[qa] level " .. name)
	end
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if name ~= "k00_marsh" then
		burst(now, after, 0, 10)
		-- the cases still bouncing and ringing when the level goes
		if stage == 10 and after(150) then
			cmd("qa_shells")
			say("[qa] loading the marsh")
			cmd("load fogqa")
			stage = 11
		end
	else
		burst(now, after, 0, 10)
		if stage == 10 and after(3000) then
			cmd("qa_shells")
			say("DA_WATER_PROBE_DONE")
			cmd("quit")
			stage = 11
		end
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
