-- The legacy teleport form in a real quest: a scene that knocks the player out and moves him into a
-- locked room, then the key that lets him out - both through set_actor_position(pos, lvid, gvid),
-- which silently did nothing while that form had no binding.
--
--   tools\qa\Run-WaterQa.ps1 -Name quest -Scenario quest_teleport -Save <save on the quest's level>
--
-- The quest functions are the add-on's own; this probe only calls them. It is QA, not the game.
local stage, t0, frames = 0, 0, 0
local passed, total = 0, 0

local ROOM = vector():set(18.442028045654, 0.88040667772293, 292.48748779297)
local OUTSIDE = vector():set(13.566362380981, 0.86585229635239, 291.75546264648)
local KEY = "object_dar2_cordon_bandit_atp_poboch_quest3_2"

local function cmd(s) get_console():execute(s) end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	printf("[qa] %s %s %s", step, ok and "PASS" or "FAIL", tostring(detail or ""))
	cmd("flush")
end

local shots = 0
local function shot()
	shots = shots + 1
	printf(string.format("[qa] SHOT %02d", shots))
	cmd("flush")
end

local function where()
	local p = db.actor:position()
	return string.format("%.2f,%.2f,%.2f", p.x, p.y, p.z)
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		printf("[qa] level %s at %s", level.name(), where())
		local ok, err = pcall(xr_effects.script_dar2_cordon_bandit_atp_poboch_quest3_1)
		check("knockout-scene-ran", ok, tostring(err))
		t0 = now stage = 1
	elseif stage == 1 and after(1500) then
		local d = db.actor:position():distance_to(ROOM)
		check("moved-into-room", d < 1.5, string.format("at %s, %.2f m from the room", where(), d))
		t0 = now stage = 2
	elseif stage == 2 and after(3500) then
		shot()
		alife():create(KEY, db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id(), db.actor:id())
		t0 = now stage = 3
	elseif stage == 3 and after(1500) then
		check("key-in-inventory", db.actor:object(KEY) ~= nil, "")
		local ok, err = pcall(xr_effects.script_dar2_cordon_bandit_atp_poboch_quest3_2)
		check("door-scene-ran", ok, tostring(err))
		t0 = now stage = 4
	elseif stage == 4 and after(1500) then
		local d = db.actor:position():distance_to(OUTSIDE)
		check("moved-outside", d < 1.5, string.format("at %s, %.2f m from the door", where(), d))
		shot()
		t0 = now stage = 5
	elseif stage == 5 and after(1300) then
		printf(string.format("[qa] DONE %d/%d", passed, total))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 6
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
