-- Landing damage through water: the same drop into deeper water, into wading water and onto the dry
-- start spot, health reset before each. The rig cannot jump, so the actor is lifted and let fall.
-- Expected (MODDING.md, Fall damage): water up to fall_water_shallow costs the dry loss, deeper
-- water less. The first seconds after a load ignore hits, so the drops start after a pause.
local stage, t0, frames = 0, 0, 0
local h0, y0, label, start = 0, 0, "", nil
local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function drop(name, height)
	db.actor:set_health_ex(1)
	local p = db.actor:position()
	h0, y0, label = db.actor.health, p.y, name
	db.actor:set_actor_position(vector():set(p.x, p.y + height, p.z))
end

local function report()
	local p = db.actor:position()
	say(string.format("[qa] FALL %s: lost %.3f health, landed %.2f m from the start height, alive %s",
		label, h0 - db.actor.health, p.y - y0, tostring(db.actor:alive())))
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 40 then return end
	local now = time_global()

	if stage == 0 then
		start = db.actor:position()
		cmd("qa_water_goto 0 1.6")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 5000 then
		drop("deeper water 16 m", 16)
		t0 = now stage = 2
	elseif stage == 2 and now - t0 > 3500 then
		report()
		cmd("qa_water_goto 0 0.7")
		t0 = now stage = 3
	elseif stage == 3 and now - t0 > 2000 then
		drop("wading water 16 m", 16)
		t0 = now stage = 4
	elseif stage == 4 and now - t0 > 3500 then
		report()
		db.actor:set_actor_position(start)
		t0 = now stage = 5
	elseif stage == 5 and now - t0 > 2000 then
		drop("dry start spot 16 m", 16)
		t0 = now stage = 6
	elseif stage == 6 and now - t0 > 3500 then
		report()
		say("DA_WATER_PROBE_DONE")
		stage = 7
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
