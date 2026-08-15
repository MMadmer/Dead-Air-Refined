local HERE = string.match(arg[0], "^(.*)[\\/][^\\/]+$") or "."
local REPO = HERE .. "\\..\\.."
local SCRIPTS = REPO .. "\\packaging\\dead-air-x64\\compatibility\\gamedata\\scripts"
local CONFIG = REPO .. "\\packaging\\dead-air-x64\\compatibility\\gamedata\\configs"
local EXAMPLES = os.getenv("NQ_EXAMPLES") or "D:\\Games\\XFined-Editor\\docs\\nq\\examples"
local mock = dofile(HERE .. "\\mock_engine.lua")
mock.setup({ scripts_dir = SCRIPTS, config_dir = CONFIG, appdata_dir = HERE,
	modules = { { id = "mod_a", root = EXAMPLES } } })
mock.fresh()
mock.add_smart("esc_smart_terrain_2_12", 100, 0, 100, 1, 20)
mock.add_smart("esc_smart_terrain_3_16", -200, 0, 50, 1, 20)
mock.add_npc("esc_2_12_stalker_wolf", "esc_2_12_stalker_wolf", { community = "stalker", name = "Wolf" })
mock.first_update()

local function hex(x)
	local t = {}
	for i = 1, math.min(#x, 24) do t[#t + 1] = string.format("%02X", string.byte(x, i)) end
	return table.concat(t, " ")
end

local u = xms_nq_util
local s = "\208\161\208\187\209\139\209\136\208\176\208\187"
io.write("to_cp1251 utf8 : ", hex(s), "\n")
io.write("to_cp1251 out  : ", hex(u.to_cp1251(s)), "\n")
io.write("language       : ", tostring(u.language()), "\n")

local q = xms_nq.quest_def("mod_a.dialog_branching")
local node = q and q.node_by_id and q.node_by_id["meet"]
io.write("topic text     : ", hex(node and node.params and node.params.text or ""), "\n")
local t = q and q.tasks and q.tasks.kill_boars
io.write("task title     : ", hex(t and t.title or ""), "\n")
for id, d in pairs(mock.dialogs_registered or {}) do
	io.write("dialog registered: ", tostring(id), "\n")
end
for _, p in ipairs(mock.phrases or {}) do
	io.write("phrase ", tostring(p.id), " : ", hex(tostring(p.text or "")), "\n")
end
