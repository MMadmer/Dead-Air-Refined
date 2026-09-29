-- Saves under protected mode (a module from the mod catalog): the game's CTime persists as Lua
-- source and must pass the marshal gate, while a function or a table's __persist stays refused;
-- a real save then goes through. Run with the rig's modules\.catalog\state.ltx at protect = 1
-- (tools\qa\Run-WaterQa.ps1 -Scenario protect_save).
local stage, t0, frames = 0, 0, 0
local passed, total = 0, 0

local function cmd(s) get_console():execute(s) end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	printf("[qa] %s %s %s", step, ok and "PASS" or "FAIL", tostring(detail or ""))
	cmd("flush")
end

local function save_exists(name)
	return getFS():exist("$game_saves$", name .. ".scop") ~= nil
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()

	if stage == 0 then
		local ok_ctime, blob = pcall(marshal.encode, { t = game.get_game_time() })
		local back = ok_ctime and marshal.decode(blob)
		local same = back and back.t and back.t:diffSec(game.get_game_time()) == 0
		check("ctime-roundtrip", ok_ctime and same, ok_ctime and "" or tostring(blob))

		local ok_fn, err_fn = pcall(marshal.encode, { f = function() end })
		check("function-refused", not ok_fn, tostring(err_fn))

		local hooked = setmetatable({}, { __persist = function() return function() return {} end end })
		local ok_tbl, err_tbl = pcall(marshal.encode, { h = hooked })
		check("table-persist-refused", not ok_tbl, tostring(err_tbl))

		-- The gate walks the whole state before the incremental encoder starts: its price.
		local state = alife_storage_manager.get_state()
		local c0 = os.clock()
		local ok_begin, err_begin = pcall(marshal.encode_begin, state)
		local ms = (os.clock() - c0) * 1000
		check("state-gate", ok_begin, string.format("%.2f ms %s", ms, ok_begin and "" or tostring(err_begin)))

		cmd("save qa_protect")
		t0 = now stage = 1
	elseif stage == 1 and now - t0 > 6000 then
		check("save-written", save_exists("qa_protect"), "")
		printf(string.format("[qa] DONE %d/%d", passed, total))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 2
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
