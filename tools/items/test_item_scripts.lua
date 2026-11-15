-- Item data in the game's windows: headless tests of xms_items.script (test tooling, not shipped).
--
--   python tools\nq\run_lua.py tools\items\test_item_scripts.lua [-v] [--stock <scripts folder> ...]
--
-- Without --stock the data readers and the upgrade fallback run against small stand-ins. Every
-- --stock folder holds the game's own ui_itm_craft.script, ui_mm_faction_select.script and
-- inventory_upgrades.script (unpacked from the game's archives; one folder per script layer): the
-- craft window, the new-game screen and the upgrade functions are then the stock code itself,
-- driven through a stand-in of the engine's UI and ALife, with the wrappers installed over it.

local HERE = string.match(arg[0], "^(.*)[\\/][^\\/]+$") or "."
local REPO = HERE .. "\\..\\.."
local SCRIPT = os.getenv("XMS_ITEMS_SCRIPT")
	or (REPO .. "\\packaging\\dead-air-x64\\compatibility\\gamedata\\scripts\\xms_items.script")

local verbose, stock_dirs = false, {}
do
	local i = 1
	while i <= #arg do
		if (arg[i] == "-v") then verbose = true
		elseif (arg[i] == "--stock") then i = i + 1; stock_dirs[#stock_dirs + 1] = arg[i]
		end
		i = i + 1
	end
end

local passed, failed = 0, 0
local function check(cond, msg)
	if (cond) then
		passed = passed + 1
		if (verbose) then io.write("  ok   ", msg, "\n") end
	else
		failed = failed + 1
		io.write("  FAIL ", msg, "\n")
	end
	return cond
end
local function section(name) io.write("== ", name, "\n") end

-- ============================================================================ the engine, faked
local G = _G
local LOG = {}
function printf(fmt, ...)
	local args, i = { ... }, 0
	local s = (select("#", ...) == 0) and tostring(fmt) or string.gsub(tostring(fmt), "%%s", function()
		i = i + 1
		return tostring(args[i])
	end)
	LOG[#LOG + 1] = s
	if (verbose) then io.write("     log: ", s, "\n") end
end
local function log_has(text)
	for _, l in ipairs(LOG) do
		if (string.find(l, text, 1, true)) then return true end
	end
	return false
end

-- An ini over a table { [section] = { key = value, ... } }. Lines come back sorted by key, as the
-- engine hands them; a strict read of what is not there is an error, as it stops the engine.
local function make_ini(name, data)
	local ini = { name = name, data = data }
	local function lines(s)
		local keys = {}
		for k in pairs(data[s] or {}) do keys[#keys + 1] = k end
		table.sort(keys)
		return keys
	end
	function ini:section_exist(s) return s ~= nil and data[s] ~= nil end
	function ini:line_exist(s, k) return data[s] ~= nil and data[s][k] ~= nil end
	function ini:line_count(s) return #lines(s) end
	function ini:r_line(s, i, a, b)
		local k = lines(s)[i + 1]
		return k ~= nil, k, data[s][k]
	end
	local function strict(s, k)
		if not (data[s]) then error("FATAL " .. name .. ": can't find section [" .. tostring(s) .. "]", 0) end
		if (data[s][k] == nil) then error("FATAL " .. name .. ": can't find [" .. s .. "] " .. k, 0) end
		return data[s][k]
	end
	function ini:r_string(s, k) return tostring(strict(s, k)) end
	function ini:r_u32(s, k) return math.floor(tonumber(strict(s, k))) end
	function ini:r_float(s, k) return tonumber(strict(s, k)) end
	function ini:r_string_ex(s, k) local v = data[s] and data[s][k]; return v ~= nil and tostring(v) or nil end
	function ini:r_float_ex(s, k) local v = data[s] and data[s][k]; return v ~= nil and tonumber(v) or nil end
	function ini:r_bool_ex(s, k) local v = data[s] and data[s][k]; return v == true or v == "true" end
	function ini:r_string_wq(s, k) return ini:r_string_ex(s, k) end
	return ini
end

local SYSTEM, FILES = nil, {}
function system_ini() return SYSTEM end
function ini_file(name)
	local data = FILES[name]
	if not (data) then error("FATAL: no file " .. tostring(name), 0) end
	return make_ini(name, data)
end

-- widgets: what a test reads back is recorded, everything else is a no-op
local Widget = {}
Widget.__index = function(t, k)
	local m = rawget(Widget, k)
	if (m) then return m end
	return function() end
end
local function widget(kind, text)
	return setmetatable({ kind = kind, text = text or "", items = {}, sel = -1 }, Widget)
end
function Widget:SetText(t) self.text = t end
function Widget:GetText() return self.text end
function Widget:SetTextColor(c) self.color = c end
function Widget:InitTexture(t) self.texture = t end
function Widget:SetTextureRect(r) self.rect = r end
function Widget:SetWndPos(v) self.pos = v end
function Widget:SetWndSize(v) self.size = v end
function Widget:GetWndPos() return self.pos or { x = 0, y = 0 } end
function Widget:GetWidth() return 100 end
function Widget:GetHeight() return 20 end
function Widget:GetCheck() return self.checked == true end
function Widget:CurrentID() return 0 end
function Widget:AddExistingItem(item) self.items[#self.items + 1] = item end
function Widget:GetSize() return #self.items end
function Widget:GetItemByIndex(i) return self.items[i + 1] end
function Widget:RemoveAll() self.items = {}; self.sel = -1 end
function Widget:RemoveItem(item)
	for i, it in ipairs(self.items) do
		if (it == item) then table.remove(self.items, i) break end
	end
	self.sel = -1
end
function Widget:GetSelectedIndex() return self.sel end
function Widget:GetSelectedItem() return self.items[self.sel + 1] end
function Widget:Select(i) self.sel = i end

local function ui_class(methods)
	local base = { __init = function() end }
	for _, m in ipairs(methods) do base[m] = function() end end
	return base
end
CUIScriptWnd = ui_class({ "SetWndRect", "SetWndPos", "SetAutoDelete", "Register", "AddCallback", "ShowDialog",
	"Enable", "Show", "Update", "OnKeyboard" })
function CUIScriptWnd:HideDialog() self.hidden = true end
function CUIScriptWnd:IsShown() return not self.hidden end
CUIListBoxItem = ui_class({ "SetTextColor", "SetWndSize", "SetFont" })
function CUIListBoxItem:__init(h) self._text = widget("text") end
function CUIListBoxItem:GetTextItem() return self._text end
function CUIListBoxItem:AddTextField(t) local w = widget("field", t); self.fields = self.fields or {}; self.fields[#self.fields + 1] = w; return w end

-- luabind's class "x" (base): a callable class table, super() inside __init
function class(name)
	local env = getfenv(2)
	return function(base)
		local C = {}
		C.__index = C
		setmetatable(C, { __index = base, __call = function(cls, ...)
			local inst = setmetatable({}, C)
			local prev = rawget(G, "super")
			G.super = function(...) if (base and base.__init) then base.__init(inst, ...) end end
			C.__init(inst, ...)
			G.super = prev
			return inst
		end })
		env[name] = C
	end
end

CScriptXmlInit = function()
	local x = {}
	return setmetatable(x, { __index = function(t, k)
		if (k == "InitListBox" or k == "InitComboBox") then return function() return widget("list") end end
		return function() return widget(k) end
	end })
end
vector = function() return { set = function(v) return v end } end
CUIWindow = function() return widget("window") end
CUITextWnd = function() return widget("text") end
CUIComboBox = function() return widget("list") end
Frect = function() return { set = function(r, a, b, c, d) r.x1, r.y1, r.x2, r.y2 = a, b, c, d; return r end } end
vector2 = function() return { set = function(v, x, y) v.x, v.y = x, y; return v end } end
GetARGB = function(a, r, g, b) return string.format("%d,%d,%d,%d", a, r, g, b) end
GetFontLetterica16Russian = function() return "f16" end
GetFontLetterica18Russian = function() return "f18" end
ui_events = setmetatable({}, { __index = function() return 0 end })
DIK_keys = setmetatable({}, { __index = function() return 0 end })
key_bindings = setmetatable({}, { __index = function() return 0 end })
dik_to_bind = function() return 0 end
strformat = string.format
IsLastStandMode = function() return false end
USE_MARSHAL = true
bit_or = function(a, b)
	-- an int on the C++ side: anything else is the corruption the kits must never cause
	assert(a == math.floor(a) and b == math.floor(b) and math.abs(a) < 2 ^ 31 and math.abs(b) < 2 ^ 31,
		"bit_or got a value that is no 32-bit int: " .. tostring(b))
	return bit.bor(a, b)
end
bit_and = function(a, b) return bit.band(a, b) end

-- ALife and the actor
local SIM = {}
local function reset_world()
	SIM = { next_id = 100, objects = {}, created = {}, released = {}, ammo = {}, effects = {} }
end
local function make_se(sec, parent)
	SIM.next_id = SIM.next_id + 1
	local se = { id = SIM.next_id, sec = sec, parent = parent }
	function se:section_name() return self.sec end
	SIM.objects[se.id] = se
	SIM.created[#SIM.created + 1] = se
	return se
end
function alife()
	return {
		create = function(_, sec, pos, lv, gv, pid) return make_se(sec, pid) end,
		create_ammo = function(_, sec, pos, lv, gv, pid, n) local se = make_se(sec, pid); se.rounds = n; return se end,
		release = function(_, se) if (se) then SIM.released[#SIM.released + 1] = se.sec; SIM.objects[se.id] = nil end end,
		actor = function() return { set_character_name = function() end, set_profile_name = function() end } end,
	}
end
function alife_object(id) return SIM.objects[id] end
function create_ammo(sec, pos, lv, gv, pid, num)
	SIM.ammo[#SIM.ammo + 1] = { sec = sec, num = num, pid = pid }
	return {}
end

local ACTOR
local function make_actor(inventory)
	local a = { inv = {}, cash = 5000, infos = {} }
	for _, sec in ipairs(inventory or {}) do
		local se = make_se(sec, 0)
		a.inv[#a.inv + 1] = { sid = se.id, sec = sec }
	end
	SIM.created = {}
	function a:iterate_inventory(fn, obj)
		for _, it in ipairs(self.inv) do
			if (SIM.objects[it.sid]) then
				fn(obj, { section = function() return it.sec end, id = function() return it.sid end })
			end
		end
	end
	function a:position() return { x = 0 } end
	function a:level_vertex_id() return 1 end
	function a:game_vertex_id() return 2 end
	function a:id() return 0 end
	function a:has_info(i) return self.infos[i] == true end
	function a:money() return self.cash end
	function a:give_money(n) self.cash = self.cash + n end
	function a:set_character_community() end
	return a
end
db = { actor = nil, storage = {}, actor_binder = {} }
level = { object_by_id = function(id) return db.storage[id] and db.storage[id].object end }
game = { translate_string = function(s) return s end }
actor_effects = { use_item = function(s) SIM.effects[#SIM.effects + 1] = s end }
alun_utils = {
	read_from_ini = function(ini, s, k, t, def)
		ini = ini or system_ini()
		if (s and k and ini:section_exist(s) and ini:line_exist(s, k)) then
			if (t == "float") then return ini:r_float_ex(s, k) end
			if (t == "bool") then return ini:r_bool_ex(s, k) end
			return ini:r_string_ex(s, k)
		end
		return def
	end,
	str_explode = function(s, sep)
		local out = {}
		for part in string.gmatch(tostring(s or "") .. sep, "([^" .. sep .. "]*)" .. sep) do
			out[#out + 1] = (string.gsub(part, "^%s*(.-)%s*$", "%1"))
		end
		return out
	end,
	collect_sections = function(ini, list)
		local out = {}
		for _, s in ipairs(list) do
			for k, v in pairs(ini.data[s] or {}) do out[k] = v end
		end
		return out
	end,
	get_communities_list = function() return {} end,
}
utils = {
	is_ammo = function(s) return string.find(s, "^ammo_") ~= nil end,
	parse_names = function(s)
		local out = {}
		for n in string.gmatch(s or "", "[%w_%.%-]+") do out[#out + 1] = n end
		return out
	end,
}
relation_registry = { community_relation = function() return 0 end, set_community_goodwill = function() end }
patrol = function() return { level_vertex_id = function() return 0 end, game_vertex_id = function() return 0 end,
	point = function() return {} end } end
give_info = function(i) if (db.actor) then db.actor.infos[i] = true end end
xr_logic = {
	parse_condlist = function(npc, s, k, v) return v end,
	pick_section_from_condlist = function(a, n, t) return t == "{+allowed}" and "true" or "false" end,
}

-- the options file of the new-game screen
local CONFIG = {}
axr_main = { config = {
	r_value = function(self, s, k, t, def)
		local v = CONFIG[s .. "." .. k]
		if (v == nil) then return def end
		if (t == 1) then return v == true or v == "true" end
		return v
	end,
	w_value = function(self, s, k, v) CONFIG[s .. "." .. k] = v end,
	save = function(self) CONFIG.saved = (CONFIG.saved or 0) + 1 end,
} }

-- RegisterScriptCallback, as axr_main keeps it
local CALLBACKS = {}
function RegisterScriptCallback(name, fn)
	CALLBACKS[name] = CALLBACKS[name] or {}
	table.insert(CALLBACKS[name], fn)
end
function UnregisterScriptCallback() end
local function send(name, ...)
	for _, fn in ipairs(CALLBACKS[name] or {}) do fn(...) end
end

xms = { modules = function() return { { id = "test_mod", enabled = true } } end }

-- a script loaded the way the game loads a namespace
local function load_script(ns, path)
	local env = setmetatable({}, { __index = G })
	G[ns] = env
	local chunk = assert(loadfile(path))
	setfenv(chunk, env)
	chunk()
	return env
end

-- a fresh Lua state: the namespaces and the callbacks go, the engine's data stays
local function new_state()
	for _, ns in ipairs({ "ui_itm_craft", "ui_mm_faction_select", "inventory_upgrades", "xms_items" }) do G[ns] = nil end
	CALLBACKS = {}
	LOG = {}
end

-- ============================================================================ the data
local function base_system()
	return {
		-- items
		jar = { inv_name = "st_jar", inv_grid_x = 1, inv_grid_y = 2, inv_grid_width = 1, inv_grid_height = 1 },
		rope = { inv_name = "st_rope", inv_grid_x = 3, inv_grid_y = 2, inv_grid_width = 1, inv_grid_height = 1 },
		synthrope = { inv_name = "st_synthrope", inv_grid_x = 0, inv_grid_y = 0, inv_grid_width = 1, inv_grid_height = 1 },
		textile_patch_b = { inv_name = "st_patch_b", inv_grid_x = 0, inv_grid_y = 0, inv_grid_width = 1, inv_grid_height = 1 },
		mod_glue = { inv_name = "st_mod_glue", icons_texture = "ui\\test_mod_icons", inv_grid_x = 2, inv_grid_y = 0,
			inv_grid_width = 1, inv_grid_height = 1 },
		wpn_sil_mod = { inv_name = "st_wpn_sil_mod", inv_grid_x = 0, inv_grid_y = 0, inv_grid_width = 2, inv_grid_height = 1 },
		ammo_9x18_fmj = { inv_name = "st_ammo_9x18", box_size = 20 },
		helm_cloth_mask = { inv_name = "st_mask" },
		itm_repairkit_tier_1 = { inv_name = "st_kit1", inv_grid_x = 5, inv_grid_y = 5, inv_grid_width = 2, inv_grid_height = 1 },
		wpn_mosin = { inv_name = "st_mosin" }, wpn_toz34 = { inv_name = "st_toz" }, wpn_bm16 = { inv_name = "st_bm16" },
		medkit = { inv_name = "st_medkit" }, novice_outfit = { inv_name = "st_novice" },
		-- recipes
		xms_craft = {
			test_mod_1 = "itm_repairkit_tier_1, wpn_sil_mod, 1, jar, rope, mod_glue, -",
			test_mod_2 = "itm_repairkit_tier_1, ammo_9x18_fmj, 40, jar, , , ",
			test_mod_3 = "verstak_weapon, wpn_sil_mod, 2, jar",
			test_mod_bad_result = "itm_repairkit_tier_1, wpn_nothing, 1, jar",
			test_mod_bad_part = "itm_repairkit_tier_1, wpn_sil_mod, 1, jar, nothing_here",
			test_mod_bad_count = "itm_repairkit_tier_1, wpn_sil_mod, two, jar",
			test_mod_too_many = "itm_repairkit_tier_1, wpn_sil_mod, 1, jar, jar, jar, jar, jar",
		},
		-- kits
		xms_start_kits = { test_mod_sniper = "", test_mod_broken = "", test_mod_cheap = "" },
		test_mod_sniper = { title = "st_test_mod_sniper", cost = 40, descr = "st_test_mod_sniper_descr",
			items = "wpn_mosin, ammo_9x18_fmj:3, wpn_toz34|wpn_bm16@0.3-0.5, medkit:2, no_such_item, novice_outfit@0.8",
			after = "test_kit_hooks.given" },
		test_mod_broken = { cost = 10, items = "medkit" },
		test_mod_cheap = { title = "st_test_mod_cheap", cost = 5, items = "medkit" },
		-- a module's upgrade: the system config has it, item_upgrades.ltx does not
		up_test_mod_barrel = { cost = 700, value = "+5", effects = "", property = "prop_damage", precondition_functor = "x" },
		prop_test_mod = { name = "st_prop_test_mod" },
		up_stock_a = { cost = 100, value = "+10" },
	}
end

FILES["item_upgrades.ltx"] = {
	up_stock_a = { cost = 100, value = "+10" },
	prop_damage = { name = "st_prop_damage" },
}
FILES["misc\\stalkers_upgrade_info.ltx"] = {
	mech_upgr = { up_test_mod_barrel = "{+allowed}", up_stock_a = "true" },
}
FILES["plugins\\faction_loadouts.ltx"] = {
	shared = { device_torch = "", money = "10000" }, stalker = { device_torch = "", money = "10000" },
	["1"] = {}, ["2"] = {}, ["3"] = {}, ["4"] = {}, ["5"] = {},
}
FILES["plugins\\faction_loadouts_debug.ltx"] = FILES["plugins\\faction_loadouts.ltx"]
FILES["plugins\\faction_start_locations.ltx"] = { stalker_start_locations = { rookie_village = "" },
	rookie_village = { lvid = 1, gvid = 2, x = 0, y = 0, z = 0 } }

test_kit_hooks = { calls = {} }
function test_kit_hooks.given(kit) table.insert(test_kit_hooks.calls, kit) end

-- ============================================================================ data readers
section("craft recipes from [xms_craft]")
SYSTEM = make_ini("system", base_system())
new_state()
local X = load_script("xms_items", SCRIPT)
do
	local list = X.craft_recipes("itm_repairkit_tier_1")
	check(#list == 2, "two usable recipes for the first repair kit, got " .. #list)
	check(list[1] and list[1].key == "test_mod_1" and list[2] and list[2].key == "test_mod_2", "in the order of their keys")
	local r = list[1]
	check(r and r.result == "wpn_sil_mod" and r.count == 1, "result and count")
	check(r and r.parts[1] == "jar" and r.parts[2] == "rope" and r.parts[3] == "mod_glue" and r.parts[4] == "-",
		"four parts, `-` kept")
	local r2 = list[2]
	check(r2 and r2.count == 40 and r2.parts[2] == "-" and r2.parts[3] == "-" and r2.parts[4] == "-", "empty parts become `-`")
	local w = X.craft_recipes("verstak_weapon")
	check(#w == 1 and w[1].parts[2] == "-" and w[1].parts[4] == "-", "parts left out at the end are `-`")
	check(#X.craft_recipes("itm_drugkit") == 0, "a tool no recipe names has none")
	check(log_has("craft recipe test_mod_bad_result is left out - the result wpn_nothing is no section"), "unknown result: one log line")
	check(log_has("craft recipe test_mod_bad_part is left out - the part nothing_here is no section"), "unknown part: one log line")
	check(log_has("craft recipe test_mod_bad_count is left out - the count is no whole number"), "bad count: one log line")
	check(log_has("craft recipe test_mod_too_many is left out - more than four parts"), "five parts: one log line")
	local n = #LOG
	X.craft_recipes("itm_repairkit_tier_1")
	check(#LOG == n, "the section is read once per Lua state")
end

section("start kits from [xms_start_kits]")
do
	local kits = X.start_kits()
	check(#kits == 2, "two usable kits, got " .. #kits)
	local cheap, sniper = kits[1], kits[2]
	check(cheap and cheap.section == "test_mod_cheap" and sniper and sniper.section == "test_mod_sniper", "in the order of their keys")
	check(cheap and cheap.index == 1001 and sniper and sniper.index == 1002, "row indices past the screen's own")
	check(log_has("start kit test_mod_broken is left out - it has no title"), "a kit without a title is left out")
	check(sniper and #sniper.rows == 5, "five rows (the unknown section left out), got " .. (sniper and #sniper.rows or 0))
	check(log_has("start kit test_mod_sniper: no_such_item is not a section of the game - left out"), "unknown section: one log line")
	local alt = sniper and sniper.rows[3]
	check(alt and #alt.sections == 2 and alt.cmin == 0.3 and alt.cmax == 0.5, "one of two, condition 0.3-0.5")
	local single = sniper and sniper.rows[5]
	check(single and single.cmin == 0.8 and single.cmax == 0.8, "a single condition")
	check(sniper and sniper.rows[2].count == 3, "a count")
	local text = sniper and X.kit_text(sniper) or ""
	check(string.find(text, "st_loadout_points 40", 1, true) and string.find(text, "st_test_mod_sniper_descr", 1, true)
		and string.find(text, "st_toz / st_bm16", 1, true) and string.find(text, "st_medkit x2", 1, true), "the kit's description")
end

-- ============================================================================ the upgrades, over stand-ins
section("upgrade costs: stand-in stock functions")
do
	new_state()
	local iu = {}
	G.inventory_upgrades = iu
	local char_ini = ini_file("item_upgrades.ltx")
	function iu.get_upgrade_cost(s) return "stock:" .. char_ini:r_u32(s, "cost") * 2 end
	function iu.precondition_functor_a(p, s) char_ini:r_u32(s, "cost"); return 0 end
	function iu.effect_functor_a(p, s, l) char_ini:r_u32(s, "cost") end
	function iu.prereq_functor_a(p, s) char_ini:r_u32(s, "cost"); return "" end
	function iu.property_functor_a(p, n) return "stock:" .. char_ini:r_string(n, "name") end
	function iu.property_functor_b(p, n) return "stock" end
	function iu.property_functor_c(p, n) return "stock" end
	function iu.mech_discount(p) iu.percent = p end
	function iu.get_possibility_string() return " - hint" end
	X = load_script("xms_items", SCRIPT)
	db.actor = make_actor()
	check(X.install_upgrades() == true, "installed")
	check(iu.get_upgrade_cost("up_stock_a") == "stock:200", "an upgrade of the file: the stock function")
	check(iu.get_upgrade_cost("up_test_mod_barrel") == "st_upgr_cost: 1400", "a module upgrade: its cost from the system config")
	iu.mech_discount(1.5)
	check(iu.percent == 1.5 and iu.get_upgrade_cost("up_test_mod_barrel") == "st_upgr_cost: 1050", "the mechanic's discount is followed")
	check(iu.property_functor_a("up_stock_a", "prop_damage") == "stock:st_prop_damage", "a property of the file: stock")
	check(iu.property_functor_a("up_stock_a, up_test_mod_barrel", "prop_damage") == "st_prop_damage +15%",
		"a property summing a module upgrade: both values")
	check(iu.property_functor_a("up_test_mod_barrel", "prop_test_mod") == "st_prop_test_mod +5%", "a module's own property")
	check(log_has("xms_items") == false or not log_has("is missing"), "no missing stock function")
end

-- ============================================================================ the stock windows
local function run_stock(dir)
	local function path(n) return dir .. "\\" .. n .. ".script" end

	-- ------------------------------------------------------------------ craft
	section("craft window (" .. dir .. ")")
	reset_world()
	SYSTEM = make_ini("system", base_system())
	new_state()
	local craft = load_script("ui_itm_craft", path("ui_itm_craft"))
	local fill0 = craft.craft_ui.FillCraftList
	local xi = load_script("xms_items", SCRIPT)
	check(xi.install_craft() == true, "installed")
	check(craft.craft_ui.FillCraftList ~= fill0, "the window's list is wrapped")

	db.actor = make_actor({ "jar", "rope" })
	local wnd = craft.craft_ui(nil, "itm_repairkit_tier_1")
	wnd:FillCraftList()
	local list = wnd.list_box_craft
	local first = wnd.xms_first
	check(first and first > 0, "the stock recipes come first: " .. tostring(first))
	check(list:GetSize() == first + 2, "two recipes from data after them")
	local row = list:GetItemByIndex(first)
	check(row and row.fn and row.fn.text == "st_wpn_sil_mod" and row.fields and row.fields[1].text == "x1",
		"a data row: the result's name and its count, like a stock row")
	check(list:GetItemByIndex(first + 1).fields[1].text == "x40", "the ammo recipe's count")

	-- a data recipe, parts missing
	list:Select(first)
	wnd:OnPartsListItemClicked()
	check(wnd.caption_part1.text == "st_jar" and wnd.caption_part3.text == "st_mod_glue", "its parts are named")
	check(wnd.caption_part1.color == "200,0,255,0" and wnd.caption_part3.color == "200,255,0,0", "found green, missing red")
	check(wnd.icon_part3.texture == "ui\\test_mod_icons", "a module part draws from its own icon sheet")
	check(wnd.icon_part1.rect and wnd.icon_part1.rect.x1 == 50 and wnd.icon_part1.rect.y1 == 100, "the part's cell")
	check(wnd.can_craft == false, "not craftable without every part")
	wnd:OnButton_ok()
	check(#SIM.created == 0 and not wnd.hidden, "nothing made")

	-- a stock recipe after it: the stock drawing, from the stock atlas again
	list:Select(0)
	wnd:OnPartsListItemClicked()
	check(wnd.icon_part3.texture == "ui\\ui_icon_equipment", "the stock row draws from the equipment atlas again")

	-- the data recipe with every part
	db.actor = make_actor({ "jar", "rope", "mod_glue", "jar" })
	wnd = craft.craft_ui(nil, "itm_repairkit_tier_1")
	wnd:FillCraftList()
	wnd.list_box_craft:Select(wnd.xms_first)
	wnd:OnPartsListItemClicked()
	check(wnd.can_craft == true and wnd.caption_part4.color == nil or wnd.can_craft == true, "craftable with every part")
	wnd:OnButton_ok()
	check(#SIM.created == 1 and SIM.created[1].sec == "wpn_sil_mod" and SIM.created[1].parent == 0, "the result made for the actor")
	table.sort(SIM.released)
	check(table.concat(SIM.released, ",") == "jar,mod_glue,rope", "exactly the three parts taken")
	check(SIM.effects[1] == "toolkit_u_dummy" and wnd.hidden, "the stock effect and the window closed")

	-- ammo: made by the round, like the stock's
	reset_world()
	db.actor = make_actor({ "jar" })
	wnd = craft.craft_ui(nil, "itm_repairkit_tier_1")
	wnd:FillCraftList()
	wnd.list_box_craft:Select(wnd.xms_first + 1)
	wnd:OnPartsListItemClicked()
	wnd:OnButton_ok()
	check(SIM.ammo[1] and SIM.ammo[1].sec == "ammo_9x18_fmj" and SIM.ammo[1].num == 40, "ammo: 40 rounds")

	-- a stock recipe still works the stock way
	reset_world()
	db.actor = make_actor({ "synthrope", "textile_patch_b", "textile_patch_b", "textile_patch_b", "textile_patch_m",
		"textile_patch_m", "textile_patch_m", "jar", "rope", "tarpaulin" })
	wnd = craft.craft_ui(nil, "itm_repairkit_tier_1")
	wnd:FillCraftList()
	local stock_ok = false
	for i = 0, wnd.xms_first - 1 do
		wnd.list_box_craft:Select(i)
		wnd:OnPartsListItemClicked()
		if (wnd.can_craft) then
			local name = wnd.list_box_craft:GetItemByIndex(i).section
			wnd:OnButton_ok()
			stock_ok = SIM.created[1] ~= nil and SIM.created[1].sec == name and #SIM.released > 0
			break
		end
	end
	check(stock_ok, "a stock recipe the inventory allows is made by the stock code")

	-- a tool no data names: the stock list alone
	wnd = craft.craft_ui(nil, "itm_drugkit")
	wnd:FillCraftList()
	check(wnd.xms_first == wnd.list_box_craft:GetSize(), "a tool without data recipes lists the stock ones only")

	-- no [xms_craft]: nothing wrapped
	local sys = base_system()
	sys.xms_craft = nil
	SYSTEM = make_ini("system", sys)
	new_state()
	craft = load_script("ui_itm_craft", path("ui_itm_craft"))
	fill0 = craft.craft_ui.FillCraftList
	xi = load_script("xms_items", SCRIPT)
	xi.install_craft()
	check(craft.craft_ui.FillCraftList == fill0 and craft.craft_ui.OnButton_ok ~= nil and not craft.xms_items_wrapped,
		"without recipes from data the window is the stock one")

	-- ------------------------------------------------------------------ the new-game screen
	section("new-game screen kits (" .. dir .. ")")
	reset_world()
	SYSTEM = make_ini("system", base_system())
	new_state()
	CONFIG = {}
	local screen = load_script("ui_mm_faction_select", path("ui_mm_faction_select"))
	xi = load_script("xms_items", SCRIPT)	-- installs as it loads, like the menu's Lua state
	check(screen.xms_items_kits == true, "installed as the script loads")
	local owner = { StartGame = function(o) o.started = true end, ShowDialog = function() end, Show = function() end }
	local ui = screen.faction_ui(owner)
	local offer = ui.list_loadout_2
	local stock_n = 0
	for i = 0, offer:GetSize() - 1 do
		if not (offer:GetItemByIndex(i).xms_kit) then stock_n = stock_n + 1 end
	end
	check(stock_n > 0 and offer:GetSize() == stock_n + 2, "the screen's own kits, then two from data")
	local kit_row = offer:GetItemByIndex(stock_n + 1)
	check(kit_row and kit_row.xms_kit and kit_row.text.text == "st_test_mod_sniper" and kit_row.points == 40,
		"a data kit: its title and its cost")
	offer:Select(stock_n + 1)
	ui:OnLoadout2Clicked()
	check(string.find(ui.loadout_desc_text.text, "st_test_mod_sniper_descr", 1, true) ~= nil, "its description pane")
	ui:OnLoadout2DbClicked()
	local picked = ui.list_loadout_1
	local moved = picked:GetItemByIndex(picked:GetSize() - 1)
	check(moved and moved.xms_kit and moved.xms_kit.section == "test_mod_sniper" and moved.text.text == "st_test_mod_sniper",
		"picked: the copy the screen makes keeps the kit")
	check(ui.loadout_points == 60, "its cost spent from the 100 points")
	-- and one of the screen's own, picked too
	offer:Select(0)
	local stock_idx = offer:GetItemByIndex(0).idx
	ui:OnLoadout2DbClicked()
	picked:Select(picked:GetSize() - 1)
	ui:OnLoadout1Clicked()
	ui.character_name:SetText("Tester")
	local ok, err = pcall(ui.OnStartGame, ui)
	check(ok, "the game starts: " .. tostring(err))
	check(owner.started == true, "the stock start ran")
	check(CONFIG["character_creation.new_game_xms_kits"] == "test_mod_sniper", "the picked data kit is stored")
	local code = tonumber(CONFIG["character_creation.new_game_loadout_code"])
	check(code == 2 ^ stock_idx + 1, "the screen's number holds its own pick and nothing past it: " .. tostring(code))
	check(moved.idx == 1002, "the row's index is back after the start")

	-- the new game loads
	new_state()
	CONFIG["character_creation.new_game_faction"] = "stalker"
	screen = load_script("ui_mm_faction_select", path("ui_mm_faction_select"))
	xi = load_script("xms_items", SCRIPT)
	db.actor = make_actor()
	reset_world()
	math.randomseed(7)
	screen.on_game_start()
	xi.on_game_start()
	test_kit_hooks.calls = {}
	-- what the kit gives, apart from what the screen's own pick gives
	local give0, from = xi.give_kit, nil
	xi.give_kit = function(kit)
		from = #SIM.created + 1
		return give0(kit)
	end
	send("on_game_load")
	local mine = {}
	for i = from or (#SIM.created + 1), #SIM.created do mine[#mine + 1] = SIM.created[i] end
	check(from ~= nil and from > 1, "the screen gave its own pick as well")
	local made = {}
	for _, se in ipairs(mine) do made[se.sec] = (made[se.sec] or 0) + 1 end
	check(made.wpn_mosin == 1 and made.ammo_9x18_fmj == 3 and made.medkit == 2 and made.novice_outfit == 1,
		"the kit's items, counted (ammo by the box)")
	check((made.wpn_toz34 or 0) + (made.wpn_bm16 or 0) == 1, "one of the two")
	local cond_ok = true
	for _, se in ipairs(mine) do
		if (se.sec == "wpn_toz34" or se.sec == "wpn_bm16") then cond_ok = cond_ok and se.condition >= 0.3 and se.condition <= 0.5 end
		if (se.sec == "novice_outfit") then cond_ok = cond_ok and math.abs(se.condition - 0.8) < 1e-9 end
		if (se.sec == "wpn_mosin") then cond_ok = cond_ok and se.condition == nil end
	end
	check(cond_ok, "conditions within their ranges, none where none is asked")
	check(test_kit_hooks.calls[1] == "test_mod_sniper", "the kit's function called with its section")
	check(CONFIG["character_creation.new_game_xms_kits"] == nil, "the key cleared")

	-- a loaded save never gets a kit
	new_state()
	CONFIG = { ["character_creation.new_game_xms_kits"] = "test_mod_sniper" }
	screen = load_script("ui_mm_faction_select", path("ui_mm_faction_select"))
	xi = load_script("xms_items", SCRIPT)
	reset_world()
	xi.on_game_start()
	send("on_game_load")
	check(#SIM.created == 0 and CONFIG["character_creation.new_game_xms_kits"] == nil, "a load: nothing given, the stale key cleared")

	-- no kits in the data: the screen is the stock one
	local sys2 = base_system()
	sys2.xms_start_kits = nil
	SYSTEM = make_ini("system", sys2)
	new_state()
	screen = load_script("ui_mm_faction_select", path("ui_mm_faction_select"))
	local init0, row0 = screen.faction_ui.InitControls, screen.set_list_text
	xi = load_script("xms_items", SCRIPT)
	check(screen.faction_ui.InitControls == init0 and screen.set_list_text == row0, "without kits nothing is wrapped")

	-- ------------------------------------------------------------------ upgrades
	section("upgrade functions (" .. dir .. ")")
	SYSTEM = make_ini("system", base_system())
	new_state()
	local iu = load_script("inventory_upgrades", path("inventory_upgrades"))
	xi = load_script("xms_items", SCRIPT)
	reset_world()
	db.actor = make_actor()
	CONFIG = {}
	xi.on_game_start()
	check(iu.xms_items_wrapped == true, "wrapped when a module is enabled")
	local victim = { section = function() return "mech" end }
	db.storage[555] = { object = victim }
	iu.victim_id = 555
	check(iu.get_upgrade_cost("up_stock_a") == "st_upgr_cost: 200", "a stock upgrade: the stock cost")
	check(iu.get_upgrade_cost("up_test_mod_barrel") == "st_upgr_cost: 1400", "a module upgrade: no stop, its own cost")
	check(iu.precondition_functor_a("", "up_test_mod_barrel") == 0, "allowed and affordable")
	db.actor.cash = 100
	check(iu.precondition_functor_a("", "up_test_mod_barrel") == 2, "too dear")
	check(string.find(iu.prereq_functor_a("", "up_test_mod_barrel"), "st_upgr_enough_money", 1, true) ~= nil, "the money hint")
	db.actor.cash = 5000
	iu.effect_functor_a("", "up_test_mod_barrel", 0)
	check(db.actor.cash == 3600, "the price paid: " .. db.actor.cash)
	iu.effect_functor_a("", "up_test_mod_barrel", 1)
	check(db.actor.cash == 3600, "nothing paid while loading")
	check(iu.precondition_functor_a("", "up_stock_a") == 0 and iu.get_upgrade_cost("up_stock_a") == "st_upgr_cost: 200",
		"stock upgrades unchanged")
	check(iu.property_functor_a("up_stock_a, up_test_mod_barrel", "prop_damage") == "st_prop_damage +15%", "a mixed property")

	-- a game without modules: the stock functions
	new_state()
	local saved = xms
	xms = { modules = function() return {} end }
	iu = load_script("inventory_upgrades", path("inventory_upgrades"))
	local f0 = iu.get_upgrade_cost
	xi = load_script("xms_items", SCRIPT)
	xi.on_game_start()
	check(iu.get_upgrade_cost == f0 and not iu.xms_items_wrapped, "no module: nothing wrapped")
	xms = saved
end

for _, dir in ipairs(stock_dirs) do
	local ok, err = pcall(run_stock, dir)
	check(ok, "the stock run of " .. dir .. " ended: " .. tostring(err))
end
if (#stock_dirs == 0) then io.write("   (no --stock folder: the stock windows were not driven)\n") end

io.write(string.format("%d passed, %d failed\n", passed, failed))
if (failed > 0) then error("item script tests failed", 0) end
