-- Stand in the scene a bug report came from and look around it.
--
-- The actor stays exactly where the reporter saved - the point is to be where they were - and
-- the camera sweeps: four frames around the horizon, one up at the sky, one down at the ground.
-- A report about something you only see from one angle ("the sky shows through the terrain")
-- otherwise depends on the save happening to face it.
--
-- qa_repro_settle: seconds to let the level finish streaming and the weather blend before the
-- first frame. qa_repro_views: how many frames around the horizon (0 = just shoot what the save
-- was looking at).
local frames, t0, stage, shot = 0, 0, 0, 0

local function cmd(s) get_console():execute(s) end

local VIEWS = {}

local function build_views()
	local n = qa_repro_views or 4
	if n <= 0 then return end
	for i = 0, n - 1 do
		VIEWS[#VIEWS + 1] = { yaw = (i / n) * math.pi * 2, pitch = 0.0 }
	end
	-- Up at the sky and down at the ground: the two directions a horizon sweep never covers.
	VIEWS[#VIEWS + 1] = { yaw = 0.0, pitch = -0.9 }
	VIEWS[#VIEWS + 1] = { yaw = 0.0, pitch = 0.9 }
end

-- Walk the actor forward a metre at a time. A report about what happens when you get close to
-- something ("the screen dims next to a building") cannot be shot from where the save left him.
local function step_forward(metres)
	local p = db.actor:position()
	local d = device().cam_dir
	local n = math.sqrt(d.x * d.x + d.z * d.z)
	if n < 0.001 then return end
	p.x = p.x + (d.x / n) * metres
	p.z = p.z + (d.z / n) * metres
	db.actor:set_actor_position(p)
end

local function look(v)
	-- Engine yaw runs the other way round from the maths convention, which is why this is
	-- negated; pitch is positive downward.
	db.actor:set_actor_direction(-v.yaw)
	local dir = device().cam_dir
	dir.y = -math.sin(v.pitch)
	local flat = math.cos(v.pitch)
	dir.x = math.sin(v.yaw) * flat
	dir.z = math.cos(v.yaw) * flat
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 180 then return end

	local now = time_global()

	if stage == 0 then
		-- A global to poke before the first frame. Some reports are about what the engine does
		-- when something asks it to - a scene starting, a window opening - and the rig has no
		-- hands to press the key with.
		if qa_repro_call and qa_repro_call ~= "" then
			local f = _G[qa_repro_call]
			if f then
				printf("[repro] calling %s", qa_repro_call)
				f()
			else
				printf("[repro] !! no global named %s", qa_repro_call)
			end
		end
		build_views()
		local p = db.actor:position()
		printf("[repro] begin level=%s at %s %s %s, time %s:%s, settle %s s",
			tostring(level.name()), tostring(math.floor(p.x)), tostring(math.floor(p.y)),
			tostring(math.floor(p.z)), tostring(level.get_time_hours()),
			tostring(level.get_time_minutes()), tostring(qa_repro_settle or 10))
		cmd("flush")
		t0 = now stage = 1

	elseif stage == 1 and now - t0 > (qa_repro_settle or 10) * 1000 then
		cmd("screenshot repro")
		shot = 0
		t0 = now stage = 2

	elseif stage == 2 and now - t0 > 900 then
		if shot >= #VIEWS then
			printf("REPRO: DONE")
			cmd("flush")
			stage = 3
			return
		end
		shot = shot + 1
		if qa_repro_step and qa_repro_step > 0 then
			-- Approach mode: same direction, a step closer each shot.
			step_forward(qa_repro_step)
		else
			look(VIEWS[shot])
		end
		-- A frame for the camera to settle before the shutter.
		t0 = now stage = 4

	elseif stage == 4 and now - t0 > 500 then
		printf("[repro] view %s of %s", tostring(shot), tostring(#VIEWS))
		cmd("screenshot repro")
		t0 = now stage = 2
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
