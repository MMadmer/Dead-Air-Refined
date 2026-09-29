-- Lists every function the custom save data would carry: protected mode (a module from the mod
-- catalog) refuses to encode functions, so one stray function anywhere in the data costs every save
-- and every level transition (tools\qa\Run-WaterQa.ps1 -Scenario save_functions).
local stage, t0, frames = 0, 0, 0
local found, walked = {}, false

local function cmd(s) get_console():execute(s) end

local function describe(f)
	local info = debug.getinfo(f, "S")
	return info and string.format("%s:%s", tostring(info.short_src), tostring(info.linedefined)) or "?"
end

-- Every path to a function value or key, and to a table whose metatable carries __persist.
local function walk(v, path, seen, depth)
	local t = type(v)
	if t == "function" then
		found[#found + 1] = path .. " = function " .. describe(v)
		return
	end
	if t ~= "table" and t ~= "userdata" then return end
	if depth > 64 or #found > 80 then return end
	local mt = debug.getmetatable(v)
	if type(mt) == "table" and rawget(mt, "__persist") ~= nil then
		found[#found + 1] = path .. " (__persist)"
	end
	if t ~= "table" or seen[v] then return end
	seen[v] = true
	for k, x in next, v do
		local kp = path .. "[" .. tostring(k) .. "]"
		if type(k) == "function" or type(k) == "table" then walk(k, kp .. "<key>", seen, depth + 1) end
		walk(x, kp, seen, depth + 1)
	end
end

local function wrap(name)
	local original = marshal[name]
	marshal[name] = function(data, ...)
		if type(data) == "table" and not walked then
			walked = true
			walk(data, "m_data", {}, 0)
		end
		return original(data, ...)
	end
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()

	if stage == 0 then
		wrap("encode_begin")
		wrap("encode")
		cmd("save qa_fnprobe")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 6000 then
		printf(string.format("[qa] walked %s, %d function path(s)", tostring(walked), #found))
		for i = 1, #found do printf("[qa] FN " .. found[i]) end
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 2
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
