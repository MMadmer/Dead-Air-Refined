-- XMS PDA layouts: headless tests of the shared reader (test tooling, not shipped). Run:
--   python tools\nq\run_lua.py tools\nq\test_pda_layout.lua [-v] [asset.pdaasset ...]
-- Reads tools\nq\fixtures\pda\courier.pdaasset through xms_pda_layout.script and checks the
-- item list, the defaults, the rows and every coded problem; extra paths are only read and
-- must come out without errors (a module's own layout, checked before a build).

local HERE = string.match(arg[0], "^(.*)[\\/][^\\/]+$") or "."
local REPO = HERE .. "\\..\\.."
local SCRIPTS = REPO .. "\\packaging\\dead-air-x64\\compatibility\\gamedata\\scripts"
local verbose = false
local extra = {}
for i = 1, #arg do
	if (arg[i] == "-v") then verbose = true else extra[#extra + 1] = arg[i] end
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

-- the reader, loaded the way the game loads a script namespace
local L = setmetatable({}, { __index = _G })
do
	local chunk = assert(loadfile(SCRIPTS .. "\\xms_pda_layout.script"))
	setfenv(chunk, L)
	chunk()
end

local function read_asset(path)
	local f = assert(io.open(path, "rb"))
	local src = f:read("*a")
	f:close()
	local chunk = assert(loadstring(src, "=" .. path))
	setfenv(chunk, {})
	return chunk()
end

local function codes(lay)
	local out = {}
	for _, p in ipairs(lay.problems) do out[p.code] = (out[p.code] or 0) + 1 end
	return out
end

local function dump(lay)
	for _, p in ipairs(lay.problems) do io.write("    ", p.code, " ", p.path, ": ", p.message, "\n") end
end

local function by_id(lay, id)
	for _, it in ipairs(lay.items) do if (it.id == id) then return it end end
end

local function argb(s)
	local r, g, b, a = string.match(s, "^#(%x%x)(%x%x)(%x%x)(%x?%x?)$")
	a = a ~= "" and tonumber(a, 16) or 255
	return ((a * 256 + tonumber(r, 16)) * 256 + tonumber(g, 16)) * 256 + tonumber(b, 16)
end

-- a copy with `patch` applied at a dotted path of the fixture (pages.1.widgets.1 ...)
local function deep(v)
	if (type(v) ~= "table") then return v end
	local o = {}
	for k, x in pairs(v) do o[k] = deep(x) end
	return o
end

local base = read_asset(HERE .. "\\fixtures\\pda\\courier.pdaasset")

section("the fixture reads clean")
local lay = L.normalize(deep(base), { lang = "rus" })
if not (check(lay.ok and lay.errors == 0, "no errors")) then dump(lay) end
check(lay.warnings == 0, "no warnings")
if (lay.warnings ~= 0) then dump(lay) end
check(lay.id == "courier" and lay.title == "Курьер", "id and the rus title")
check(L.normalize(deep(base), { lang = "eng" }).title == "Courier", "the eng title for an English game")
check(lay.canvas.w == 960 and lay.canvas.h == 680, "the default canvas")
check(lay.start == "orders", "start page")
check(lay.keys.map == "page:map" and lay.keys.tasks == "page:orders" and lay.keys.contacts == "stock:eptContacts",
	"keys: map named, tasks and contacts defaulted")
check(#lay.pages == 3 and lay.pages[3].stock == "eptTasks", "three pages, the journal is a stock page")
check(lay.vars.current == "Нет заказа", "vars keep their text")
check(lay.device.noise == true and lay.device.close == true, "device parts kept by default")

section("items")
local kinds = {}
for _, it in ipairs(lay.items) do kinds[it.kind] = (kinds[it.kind] or 0) + 1 end
check(kinds.rect == 3 and kinds.map == 1 and kinds.list == 1 and kinds.row == 2, "rects, the map, the list and two rows")
for i, it in ipairs(lay.items) do
	if not (check(it.n == i and it.parent < i, "item " .. i .. " comes after its parent")) then break end
end
local tabs = {}
for _, it in ipairs(lay.items) do
	if (it.kind == "button" and it.layer == "chrome" and it.icon) then tabs[#tabs + 1] = it end
end
check(#tabs == 3, "three tabs")
check(tabs[1].action[1] == "page:orders" and tabs[1].selected_on[1] == "orders", "page = shorthand: action and selection")
check(tabs[1].text_selected == argb("#FFD21E") and tabs[1].icon_selected == argb("#FFD21E"), "style colours resolved")
check(tabs[1].color == 0, "none is transparent")
check(tabs[1].align == "c" and tabs[1].valign == "b", "alignment from the style")
local title = lay.items[2]
check(title.kind == "text" and title.font == "letterica25" and title.text == "Курьер", "a style gives the kind")
local card = by_id(lay, "card")
check(card and card.border == argb("#9AA0AB") and card.border_w == 2, "a border defaults to 2")
local primary
for _, it in ipairs(lay.items) do if (it.parent == card.n and it.kind == "button") then primary = it end end
check(primary and primary.hover == L.shade(argb("#FFD21E"), 0.12), "hover derived from the colour")
check(primary.action[1] == "signal:courier_done" and primary.action[2] == "var:current=Нет заказа", "actions in order")
local details = by_id(lay, "details")
check(details and details.visible == false and details.layer == "overlay", "the overlay starts hidden")
check(by_id(lay, "orders").empty == "No orders", "list: the empty line")

section("rows")
local list = by_id(lay, "orders")
local rows = {}
for _, it in ipairs(lay.items) do if (it.kind == "row") then rows[#rows + 1] = it end end
check(rows[1].parent == list.n and rows[2].parent == list.n, "rows hang under their list")
check(rows[1].row == 1 and rows[1].h == 80 and rows[1].color == argb("#1E2026"), "row geometry and colour")
check(rows[1].action[1] == "var:picked=Тушёнка" and rows[1].action[2] == "show:details", "row action with its field")
local texts = {}
for _, it in ipairs(lay.items) do
	if (it.parent == rows[1].n) then texts[#texts + 1] = it end
end
check(#texts == 5, "the template under the first row")
check(texts[1].color == argb("#FFD21E1F"), "a {field} colour names the palette")
check(texts[2].texture == "ui_xms_icon_box", "a {field} texture")
check(texts[4].text == "Ангар — Костёр" and texts[5].text == "450 RU", "{fields} in texts, numbers too")
local live = L.list_rows(lay, "orders", { { title = "Сухпай", from = "A", to = "B", price = "9", tile = "soft", icon = "ui_xms_icon_bag" } })
check(live and #live == 6 and live[1].kind == "row" and live[1].parent == 0, "rows at run time: the row under the list")
check(live[2].parent == 1 and live[4].text == "Сухпай", "rows at run time: the template under the row")
check(L.list_rows(lay, "nope", {}) == nil, "rows of a list that does not exist")

section("problems")
local function case(name, mutate, want, lang)
	local a = deep(base)
	mutate(a)
	local r = L.normalize(a, { lang = lang or "rus" })
	local c = codes(r)
	local ok = true
	for code, n in pairs(want) do ok = ok and (c[code] or 0) == n end
	if not (check(ok, name)) then dump(r) end
	return r
end
case("E301 header", function(a) a.pda = 2 end, { E301 = 1 })
case("E302 id", function(a) a.id = "Bad Id" end, { E302 = 1 })
case("E303 no pages", function(a) a.pages = {} end, { E303 = 1 })
case("E304 duplicate page", function(a) a.pages[2].id = "orders" end, { E304 = 1 })
case("E305 unknown kind", function(a) a.chrome[1].kind = "circle" end, { E305 = 1 })
case("E306 geometry", function(a) a.chrome[1].w = nil end, { E306 = 1 })
case("E307 duplicate id", function(a) a.pages[2].widgets[3].id = "orders" end, { E307 = 1 })
case("E308 page", function(a) a.chrome[4].page = "nowhere" end, { E308 = 1 })
case("E308 show", function(a) a.pages[1].widgets[1].action = "show:ghost" end, { E308 = 1 })
case("E308 verb", function(a) a.pages[2].widgets[2].action = "teleport:x" end, { E308 = 1 })
case("E308 var of the game", function(a) a.pages[2].widgets[2].action = "var:time=1" end, { E308 = 1 })
case("E309 start", function(a) a.start = "nowhere" end, { E309 = 1 })
case("E310 style", function(a) a.chrome[2].style = "nope" end, { E310 = 1 })
case("E311 palette", function(a) a.colors.bg = "red" end, { E311 = 1 })
case("E311 colour", function(a) a.chrome[1].color = "#12345" end, { E311 = 1 })
case("E312 list without an item", function(a) a.pages[1].widgets[1].item = nil end, { E312 = 1 })
case("E313 second map", function(a)
	table.insert(a.pages[1].widgets, { kind = "map", x = 0, y = 0, w = 10, h = 10 })
end, { E313 = 1 })
case("E314 map in a row", function(a)
	table.insert(a.pages[1].widgets[1].item, { kind = "map", x = 0, y = 0, w = 10, h = 10 })
end, { E314 = 1 })
case("W315 stock section", function(a) a.pages[3].stock = "eptNope" end, { W315 = 1 })
case("W316 font", function(a) a.chrome[3].font = "comic_sans" end, { W316 = 1 })
case("W317 off the canvas", function(a) a.chrome[1].w = 2000 end, { W317 = 1 })
case("W318 unknown key", function(a) a.chrome[1].colour = "#FFFFFF" end, { W318 = 1 })
case("W319 unknown var", function(a) a.chrome[3].text = "{nothing}" end, { W319 = 1 })
case("W321 button without an action", function(a) a.pages[2].widgets[2].action = nil a.pages[2].widgets[2].hint = nil end, { W321 = 1 })
local e = case("errors make the layout unusable", function(a) a.chrome[1].kind = "circle" end, { E305 = 1 })
check(e.ok == false, "ok is false with an error")

section("the file sandbox")
do
	local ok = pcall(function()
		local chunk = loadstring("os.exit(1) return {}")
		setfenv(chunk, {})
		chunk()
	end)
	check(not ok, "a layout cannot call anything")
end

for _, path in ipairs(extra) do
	section("asset " .. path)
	local ok, asset = pcall(read_asset, path)
	if (check(ok, "reads")) then
		local r = L.normalize(asset, { lang = "rus" })
		if not (check(r.ok and r.errors == 0, "no errors")) then dump(r) end
		if (r.warnings > 0) then dump(r) end
		io.write(string.format("    %d item(s), %d page(s), %d warning(s)\n", #r.items, #r.pages, r.warnings))
	end
end

io.write(string.format("pda layout tests: %d passed, %d failed\n", passed, failed))
os.exit(failed == 0 and 0 or 1)
