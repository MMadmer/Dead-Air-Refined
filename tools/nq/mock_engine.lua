-- Fake X-Ray / XMS / CoC surface for driving the NQ runtime headlessly (test tooling, not shipped).
-- Just enough for xms_nq*.script: actor with inventory/money/position/tasks, alife create/object/
-- release, level, game time, news, ini over the real catalog.ltx, story objects, SIMBOARD with
-- smarts and squads, dialogs (CPhraseDialog graph recorder + a talk session that drives
-- pre/act/text the way the engine does), CGameTask, relations, xr_sound, marshal, script
-- namespaces with autoload, RegisterScriptCallback/SendScriptCallback.
--
-- Usage from a test:
--   local mock = dofile("mock_engine.lua")
--   mock.setup({ scripts_dir = ..., config_dir = ..., modules = { {id="mod_a", root=...}, ... } })
--   mock.first_update()      -- runs xms_nq.on_game_start + actor_on_first_update
--   mock.tick(ms)            -- advances real time and sends actor_on_update
--   mock.rebuild()           -- new "Lua state": drops script tables/registries/callbacks
--   mock.talk_open(npc) / talk_topics() / talk_start(id) / talk_say(phrase) / talk_close()

local mock = {}
mock.log = {}
mock.news = {}
mock.sounds = {}
mock.spots = {}
mock.callbacks = {}
mock.blobs = {}
mock.save_calls = 0
mock.real_ms = 1000
mock.game_secs = 0
mock.next_id = 100
mock.se = {}				-- id -> server object
mock.go = {}				-- id -> game object
mock.zones = {}
mock.overrides = {}			-- "<module>/<relpath>" -> text (asset overrides for reload tests)
mock.deleted = {}			-- "<module>/<relpath>" -> true
mock.time_hours = 12
mock.opts = {}
mock.list_files_missing = false
mock.dialogs_registered = {}	-- dialog id -> init function name (xms.dialog_register)
mock.dialog_shared = {}		-- dialog id -> recorded phrase graph (CSharedClass: outlives Lua states)
mock.dialog_invalidated = {}	-- dialog id -> count
mock.dialog_loads = 0
mock.talk = nil				-- current talk session
mock.talking = false
mock.forced_talks = {}
mock.squad_relations = {}
mock.faction_relations = {}
mock.faction_goodwill = {}
mock.safe_released = {}
mock.setup_calls = 0
mock.squad_size = 2
mock.squads_online = true

-- ---------------------------------------------------------------------------- logging
-- Same contract as _g.printf: only %s is substituted, and only when arguments are given.
local function fmt_printf(fmt, ...)
	if (select("#", ...) == 0) then return tostring(fmt) end
	local args = { ... }
	local i = 0
	local s = string.gsub(tostring(fmt), "%%s", function()
		i = i + 1
		return tostring(args[i])
	end)
	return s
end

function printf(fmt, ...)
	local s = fmt_printf(fmt, ...)
	mock.log[#mock.log + 1] = s
	if (mock.verbose) then io.write(s, "\n") end
end

function log(s) printf("%s", s) end

function mock.log_has(pattern)
	for _, line in ipairs(mock.log) do
		if (string.find(line, pattern)) then return true, line end
	end
	return false
end

function mock.log_count(pattern)
	local n = 0
	for _, line in ipairs(mock.log) do
		if (string.find(line, pattern)) then n = n + 1 end
	end
	return n
end

function mock.clear_log() mock.log = {} end

-- ---------------------------------------------------------------------------- tiny marshal
local function ser(v, out)
	local t = type(v)
	if (t == "string") then out[#out + 1] = string.format("%q", v)
	elseif (t == "number") then out[#out + 1] = string.format("%.17g", v)
	elseif (t == "boolean") then out[#out + 1] = tostring(v)
	elseif (t == "table") then
		out[#out + 1] = "{"
		for k, x in pairs(v) do
			local tk = type(k)
			if (tk == "string") then out[#out + 1] = "[" .. string.format("%q", k) .. "]="
			elseif (tk == "number" or tk == "boolean") then out[#out + 1] = "[" .. tostring(k) .. "]="
			else error("marshal: bad key type " .. tk) end
			ser(x, out)
			out[#out + 1] = ","
		end
		out[#out + 1] = "}"
	elseif (t == "nil") then out[#out + 1] = "nil"
	else error("marshal: cannot encode " .. t)
	end
end

marshal = {
	encode = function(v) local out = { "return " } ser(v, out) return table.concat(out) end,
	decode = function(s)
		local f = assert(loadstring(s))
		setfenv(f, {})
		return f()
	end,
	clone = function(v) return marshal.decode(marshal.encode(v)) end,
}

-- ---------------------------------------------------------------------------- vectors, time
local vector_mt = {}
vector_mt.__index = vector_mt
function vector_mt:set(x, y, z)
	if (type(x) == "table") then self.x, self.y, self.z = x.x, x.y, x.z else self.x, self.y, self.z = x, y, z end
	return self
end
function vector_mt:distance_to(o)
	local dx, dy, dz = self.x - o.x, self.y - o.y, self.z - o.z
	return math.sqrt(dx * dx + dy * dy + dz * dz)
end
function vector_mt:distance_to_sqr(o)
	local dx, dy, dz = self.x - o.x, self.y - o.y, self.z - o.z
	return dx * dx + dy * dy + dz * dz
end
function vector() return setmetatable({ x = 0, y = 0, z = 0 }, vector_mt) end

function time_global() return mock.real_ms end

local ctime_mt = {}
ctime_mt.__index = ctime_mt
-- Y,M,D,h,m,s,ms from an absolute second count (civil calendar, epoch 2012-01-01)
local function civil_from_days(z)
	z = z + 719468
	local era = math.floor(z / 146097)
	local doe = z - era * 146097
	local yoe = math.floor((doe - math.floor(doe / 1460) + math.floor(doe / 36524) - math.floor(doe / 146096)) / 365)
	local y = yoe + era * 400
	local doy = doe - (365 * yoe + math.floor(yoe / 4) - math.floor(yoe / 100))
	local mp = math.floor((5 * doy + 2) / 153)
	local d = doy - math.floor((153 * mp + 2) / 5) + 1
	local m = mp < 10 and mp + 3 or mp - 9
	if (m <= 2) then y = y + 1 end
	return y, m, d
end
local EPOCH_DAYS = 15340 -- days from 1970-01-01 to 2012-01-01
function ctime_mt:get()
	local total = self.secs
	local days = math.floor(total / 86400)
	local rem = total - days * 86400
	local Y, M, D = civil_from_days(days + EPOCH_DAYS)
	local h = math.floor(rem / 3600)
	local m = math.floor((rem - h * 3600) / 60)
	local s = math.floor(rem - h * 3600 - m * 60)
	local ms = math.floor((rem - math.floor(rem)) * 1000)
	return Y, M, D, h, m, s, ms
end
function ctime_mt:diffSec(o) return self.secs - o.secs end
function ctime_mt:set() end
local function ctime(secs) return setmetatable({ secs = secs or 0 }, ctime_mt) end

game = {
	get_game_time = function() return ctime(mock.game_secs) end,
	CTime = function() return ctime(0) end,
	translate_string = function(s) return s end,
}

-- ---------------------------------------------------------------------------- objects
local go_mt = {}
go_mt.__index = go_mt
function go_mt:id() return self._id end
function go_mt:section() return self._section end
function go_mt:name() return self._name or self._section end
function go_mt:position() return self._pos end
function go_mt:level_vertex_id() return self._lvid or 1 end
function go_mt:game_vertex_id() return self._gvid or 1 end
function go_mt:alive() return self._alive ~= false end
function go_mt:character_community() return self._community end
function go_mt:profile_name() return self._profile end
function go_mt:character_name() return self._name or "npc" end
function go_mt:clsid() return self._clsid or 0 end
function go_mt:relation(other) return self._relation or 1 end
function go_mt:is_talking() return mock.talking == true end
function go_mt:inside(pos) return self._inside == true end
function go_mt:force_set_goodwill(gw, who) self._goodwill = gw end
function go_mt:change_goodwill(delta, who) self._goodwill = (self._goodwill or 0) + delta end
function go_mt:set_relation(rel, who) self._relation = rel end
function go_mt:general_goodwill(who) return self._goodwill or 0 end
function go_mt:get_start_dialog() return self._start_dialog end
function go_mt:set_start_dialog(id) self._start_dialog = id end
function go_mt:restore_default_start_dialog() self._start_dialog = nil end
function go_mt:stop_talk() if (mock.talk) then mock.talk_close() end end
-- death: xr_motivator death_callback -> npc_on_death_callback(victim, who), then the server side
function go_mt:kill(who)
	self._alive = false
	local se = mock.se[self._id]
	if (se) then se._alive = false end
	SendScriptCallback("npc_on_death_callback", self, who)
	if (se and se.group_id) then
		local squad = mock.se[se.group_id]
		if (squad and squad._members) then mock.squad_member_died(squad, self._id, who) end
	end
end

local function new_go(section, fields)
	local go = setmetatable({ _id = mock.next_id, _section = section, _pos = vector():set(0, 0, 0) }, go_mt)
	mock.next_id = mock.next_id + 1
	for k, v in pairs(fields or {}) do go["_" .. k] = v end
	mock.go[go._id] = go
	return go
end
mock.new_go = new_go

local se_mt = {}
se_mt.__index = se_mt
function se_mt:section_name() return self._section end
function se_mt:name() return self._name or self._section end
function se_mt:alive() return self._alive ~= false end
function se_mt:community() return self._community end
function se_mt:profile_name() return self._profile end
function se_mt:clsid() return self._clsid or 0 end
function se_mt:force_set_goodwill(gw, who_id) self._goodwill = gw end
function se_mt:kill() self._alive = false local go = mock.go[self.id] if (go) then go._alive = false end end

local function new_se(section, pos, lvid, gvid, parent, id)
	local se = setmetatable({ id = id or mock.next_id, _section = section, position = pos or vector():set(0, 0, 0), m_level_vertex_id = lvid or 1, m_game_vertex_id = gvid or 1, parent_id = parent, online = true }, se_mt)
	if not (id) then mock.next_id = mock.next_id + 1 end
	-- a spawned ammo object is one full box; create_ammo overwrites it with its own round count
	if (section and system_ini():line_exist(section, "box_size")) then se.ammo_left = system_ini():r_u32(section, "box_size") end
	mock.se[se.id] = se
	-- parent/children mirror the server side: an item names its owner, the owner lists its items
	local owner = parent and mock.se[parent]
	if (owner) then
		owner._children = owner._children or {}
		owner._children[#owner._children + 1] = se.id
	end
	return se
end
mock.new_se = new_se

local function detach_from_parent(se)
	local owner = se and se.parent_id and mock.se[se.parent_id]
	if not (owner and owner._children) then return end
	for i, id in ipairs(owner._children) do
		if (id == se.id) then table.remove(owner._children, i) break end
	end
end

-- Ownership transfer (looting a stash, handing an item over): exactly what the server does.
function mock.reparent(se, parent)
	detach_from_parent(se)
	se.parent_id = parent
	local owner = parent and mock.se[parent]
	if (owner) then
		owner._children = owner._children or {}
		owner._children[#owner._children + 1] = se.id
	end
end

-- An online stalker with a server object of the same id, in db.storage, optionally with a story id.
function mock.add_npc(section, story_id, fields)
	local go = new_go(section, fields)
	if (go._stalker == nil) then go._stalker = true end
	go._alive = true
	local se = new_se(section, go._pos, 1, 1, nil, go._id)
	se._alive = true
	se._community = go._community
	se._profile = go._profile
	se._name = go._name
	db.storage[go._id] = { object = go }
	if (story_id) then mock.sor:register(go._id, story_id) end
	return go, se
end

-- ---------------------------------------------------------------------------- actor
local actor_mt = setmetatable({}, { __index = go_mt })
actor_mt.__index = actor_mt
function actor_mt:money() return self._money end
function actor_mt:give_money(n) self._money = self._money + n end
function actor_mt:transfer_money(n, victim) self._money = self._money - n end
function actor_mt:iterate_inventory(fn, obj)
	for _, item in ipairs(self._inv) do
		if (fn(obj, item)) then return end
	end
end
function actor_mt:object(section)
	for _, item in ipairs(self._inv) do if (item:section() == section) then return item end end
	return nil
end
function actor_mt:give_info_portion(info) self._infos[info] = true SendScriptCallback("actor_on_info_callback", self, info) end
function actor_mt:disable_info_portion(info) self._infos[info] = nil end
function actor_mt:has_info(info) return self._infos[info] == true end
function actor_mt:set_actor_position(pos) self._pos = vector():set(pos) end
function actor_mt:give_game_news(caption, text, tex, tm, show) mock.news[#mock.news + 1] = { caption = caption, text = text } end
function actor_mt:give_talk_message2(caption, text) mock.news[#mock.news + 1] = { caption = caption, text = text } end
function actor_mt:transfer_item(item, to) mock.remove_item(item) end
function actor_mt:character_community() return self._community or "stalker" end
function actor_mt:run_talk_dialog(npc, disable_break)
	mock.forced_talks[#mock.forced_talks + 1] = { id = npc:id(), disable_break = disable_break }
	mock.talk_open(npc)
end
-- CGameTaskManager: HasGameTask(id, only_inprocess) / GiveGameTaskToActor / SetTaskState
-- The native manager immediately forwards new/completed states to actor_on_task_callback.
-- Dynamic NQ tasks are absent from task_manager, so failures are deliberately silent here.
local function task_state_changed(t, state)
	if (state ~= task.fail) then
		news_manager.send_task(db.actor, state == task.completed and "complete" or "new", t)
	end
end

function actor_mt:get_task(id, only_inprocess)
	for _, t in ipairs(self._tasks) do
		if (t._id == id and (not only_inprocess or t._state == 1)) then return t end
	end
	return nil
end
function actor_mt:give_task(t, dt, check_existing, ttl)
	if (self:get_task(t:get_id(), true)) then printf("! task [%s] already inprocess", t:get_id()) return end
	t._state = 1
	self._tasks[#self._tasks + 1] = t
	mock.task_events[#mock.task_events + 1] = { "given", t:get_id() }
	task_state_changed(t, task.in_progress)
end
function actor_mt:set_task_state(state, id, objective_id)
	local t = self:get_task(id, false)
	if not (t) then printf("! actor does not has task [%s]", id) return end
	-- index 0 is the task itself; anything else is one of its steps and leaves the
	-- task's own state alone, which is the whole point of sub-objectives
	if (objective_id and objective_id ~= 0) then
		local o = t._objectives and t._objectives[objective_id]
		if not (o) then printf("! task [%s] has no objective [%s]", id, tostring(objective_id)) return end
		o._state = state
		if (state == 0 or state == 2) then o:remove_map_locations(false) end
		mock.task_events[#mock.task_events + 1] =
			{ state == 2 and "objective_completed" or state == 0 and "objective_failed" or tostring(state), id, objective_id }
		return
	end
	t._state = state
	if (state == 0 or state == 2) then t:remove_map_locations(false) end
	mock.task_events[#mock.task_events + 1] = { state == 2 and "completed" or state == 0 and "failed" or tostring(state), id }
	task_state_changed(t, state)
end
function actor_mt:get_task_state(id, objective_id)
	local t = self:get_task(id, false)
	if not (t) then return 65535 end
	if (objective_id and objective_id ~= 0) then
		local o = t._objectives and t._objectives[objective_id]
		return o and o._state or 65535
	end
	return t._state
end

local function make_actor()
	local a = setmetatable({ _id = 0, _section = "actor", _pos = vector():set(0, 0, 0), _money = 1000, _inv = {}, _infos = {}, _name = "Strelok", _lvid = 1, _gvid = 1, _tasks = {} }, actor_mt)
	return a
end

-- A named space_restrictor in the world; `online` also puts it in db.zone_by_name the
-- way bind_restrictor does on net_spawn.
function mock.add_restrictor(name, pos, online, radius)
	local se = new_se("space_restrictor", pos or vector():set(0, 0, 0), 1, 1, nil)
	se._name = name
	if (online) then
		local go = new_go("space_restrictor")
		go._id = se.id
		go._name = name
		go._radius = radius or 5
		go.inside = function(self, p)
			return se.position:distance_to(p) <= (self._radius or 5)
		end
		mock.go[se.id] = go
		db.zone_by_name = db.zone_by_name or {}
		db.zone_by_name[name] = go
	end
	return se
end

-- Simulates an old save whose actor registry never saw the runtime's tasks.
function mock.tasks_lost() db.actor._tasks = {} end
function mock.task_by_id(id) return db.actor:get_task(id, false) end
mock.task_events = {}

-- ---------------------------------------------------------------------------- CGameTask
task = { additional = 1, storyline = 0, completed = 2, fail = 0, in_progress = 1, task_dummy = 65535 }
local gt_mt = {}
gt_mt.__index = gt_mt
function CGameTask() return setmetatable({ _id = "", _title = "", _descr = "", _type = 1, _prio = 0, _state = 65535 }, gt_mt) end
function gt_mt:get_id() return self._id end
function gt_mt:set_id(id) self._id = id end
function gt_mt:get_type() return self._type end
function gt_mt:set_type(t) self._type = t end
function gt_mt:get_title() return self._title end
function gt_mt:set_title(s) self._title = s end
function gt_mt:get_description() return self._descr end
function gt_mt:set_description(s) self._descr = s end
function gt_mt:get_priority() return self._prio end
function gt_mt:set_priority(p) self._prio = p end
function gt_mt:get_icon_name() return self._icon end
function gt_mt:set_icon_name(s) self._icon = s end
function gt_mt:get_state() return self._state end
function gt_mt:get_map_location() return self._map_loc end
function gt_mt:set_map_location(s) self._map_loc = s end
function gt_mt:get_map_object_id() return self._map_obj or 65535 end
function gt_mt:set_map_object_id(id) self._map_obj = id end
function gt_mt:remove_map_locations(notify) self._map_loc = nil self._map_obj = nil end
function gt_mt:change_map_location(spot, id) self._map_loc = spot self._map_obj = id self._state = 1 end
function gt_mt:add_complete_func(s) end
function gt_mt:add_fail_func(s) end

-- SGameTaskObjective: a step of a task, with its own text and its own map spot.
local ob_mt = {}
ob_mt.__index = ob_mt
function SGameTaskObjective(task_obj, idx)
	return setmetatable({ _task = task_obj, _idx = idx, _title = "", _descr = "", _state = 1 }, ob_mt)
end
function ob_mt:get_idx() return self._idx end
function ob_mt:get_title() return self._title end
function ob_mt:set_title(s) self._title = s end
function ob_mt:get_description() return self._descr end
function ob_mt:set_description(s) self._descr = s end
function ob_mt:get_state() return self._state end
function ob_mt:get_map_location() return self._map_loc end
function ob_mt:set_map_location(s) self._map_loc = s end
function ob_mt:get_map_object_id() return self._map_obj or 65535 end
function ob_mt:set_map_object_id(id) self._map_obj = id end
function ob_mt:set_map_hint(s) self._hint = s end
function ob_mt:set_icon_name(s) self._icon = s end
function ob_mt:remove_map_locations(notify) self._map_loc = nil self._map_obj = nil end
function ob_mt:change_map_location(spot, id) self._map_loc = spot self._map_obj = id end
-- a hidden step still runs, it just takes no room in the PDA
function ob_mt:is_visible() return self._hidden ~= true end
function ob_mt:set_visible(v) self._hidden = not v end
function ob_mt:create_map_location(on_load) self._spot_made = true end

function gt_mt:add_objective(o)
	self._objectives = self._objectives or {}
	self._objectives[o._idx] = o
end
function gt_mt:get_objective(idx) return self._objectives and self._objectives[idx] or nil end
function gt_mt:get_objectives_cnt()
	local n = 0
	for _ in pairs(self._objectives or {}) do n = n + 1 end
	return n
end
function gt_mt:get_active_objective() return self._active or 0 end
function gt_mt:set_active_objective(idx) self._active = idx end

-- The engine writes a task into the save but not its steps, so a task that came back from a
-- load has none. mock.drop_objectives is that load, for tests of the runtime's repair path.
function mock.drop_objectives(task_id)
	local t = mock.task_by_id(task_id)
	if (t) then t._objectives = nil t._active = nil end
	return t
end

-- Adds an item to the actor inventory (server + client objects); fires actor_on_item_take when asked.
function mock.add_item(section, notify)
	local a = db.actor
	local se = new_se(section, a:position(), 1, 1, 0)
	local go = new_go(section)
	go._id = se.id
	mock.go[se.id] = go
	a._inv[#a._inv + 1] = go
	if (notify) then SendScriptCallback("actor_on_item_take", go) end
	return go
end

function mock.remove_item(go)
	local a = db.actor
	for i, it in ipairs(a._inv) do
		if (it == go or it:id() == go:id()) then table.remove(a._inv, i) break end
	end
	detach_from_parent(mock.se[go:id()])
	mock.se[go:id()] = nil
	mock.go[go:id()] = nil
end

-- ---------------------------------------------------------------------------- containers
-- A stash / inventory box: a server object that owns its contents through parent_id. `online`
-- also gives it a client object, the way a box on the current level has one.
function mock.add_container(section, story_id, online)
	local se = new_se(section or "inventory_box", vector():set(0, 0, 0), 1, 1, nil)
	se._children = {}
	if (online) then
		local go = new_go(section or "inventory_box")
		mock.go[go._id] = nil
		go._id = se.id
		mock.go[se.id] = go
		db.storage[se.id] = { object = go }
	end
	if (story_id) then mock.sor:register(se.id, story_id) end
	return se
end

function mock.put_in_container(box, section)
	return new_se(section, box.position, 1, 1, box.id)
end

-- Moves an existing world object into the actor's inventory the way the engine does: the server
-- object changes owner, a client object joins the inventory list, actor_on_item_take fires.
function mock.pick_up(se, notify)
	mock.reparent(se, db.actor:id())
	local go = mock.go[se.id]
	if not (go) then
		go = new_go(se:section_name())
		mock.go[go._id] = nil
		go._id = se.id
		mock.go[se.id] = go
	end
	db.actor._inv[#db.actor._inv + 1] = go
	if (notify ~= false) then SendScriptCallback("actor_on_item_take", go) end
	return go
end

function mock.loot(box, section, notify)
	for _, id in ipairs(box._children or {}) do
		local se = mock.se[id]
		if (se and se:section_name() == section) then return mock.pick_up(se, notify) end
	end
	error("loot: no " .. tostring(section) .. " in the container")
end

function mock.count_items(section)
	local n = 0
	for _, it in ipairs(db.actor._inv) do if (it:section() == section) then n = n + 1 end end
	return n
end

-- Rounds the actor carries in an ammo section (each object holds ammo_left of them).
function mock.ammo_rounds(section)
	local n = 0
	for _, it in ipairs(db.actor._inv) do
		local se = mock.se[it:id()]
		if (se and se:section_name() == section) then n = n + (se.ammo_left or 0) end
	end
	return n
end

function mock.use_item(section)
	local go = db.actor:object(section)
	if not (go) then error("use_item: no " .. section) end
	SendScriptCallback("actor_on_item_use", go, section)
	mock.remove_item(go)
end

function mock.drop_item(section)
	local go = db.actor:object(section)
	if not (go) then error("drop_item: no " .. section) end
	mock.remove_item(go)
	SendScriptCallback("actor_on_item_drop", go)
end

-- ---------------------------------------------------------------------------- alife / level / db
local alife_obj = {}
-- by id, or by name_replace like the engine's alife():object(pcstr) overload - which
-- is how a restrictor is found while it is still offline
function alife_obj:object(id)
	if (type(id) == "string") then
		for _, se in pairs(mock.se) do
			if (se:name() == id) then return se end
		end
		return nil
	end
	return mock.se[id]
end
function alife_obj:create(section, pos, lvid, gvid, parent, reg)
	local se = new_se(section, pos, lvid, gvid, parent)
	if (parent == 0 and db.actor) then
		-- item created on the actor lands in the inventory
		local go = new_go(section)
		go._id = se.id
		mock.go[se.id] = go
		db.actor._inv[#db.actor._inv + 1] = go
	end
	mock.created = mock.created or {}
	mock.created[#mock.created + 1] = { section = section, parent = parent, id = se.id }
	return se
end
function alife_obj:create_ammo(section, pos, lvid, gvid, parent, num)
	local se = self:create(section, pos, lvid, gvid, parent)
	se.ammo_left = num
	return se
end
-- Alundaio's binding: return_stl_iterator over the server-side child ids of an object.
function alife_obj:get_children(se)
	local list, i = (se and se._children) or {}, 0
	return function()
		i = i + 1
		return list[i]
	end
end
function alife_obj:release(se, b)
	if (se) then
		detach_from_parent(se)
		mock.se[se.id] = nil
		if (mock.go[se.id] and db.actor) then mock.remove_item(mock.go[se.id]) end
		mock.go[se.id] = nil
		mock.released = mock.released or {}
		mock.released[#mock.released + 1] = se.id
	end
end
-- The engine's own way of confining an NPC to a zone (alife_simulator_script).
function alife_obj:add_in_restriction(se, zone_id)
	se._in_restr = se._in_restr or {}
	se._in_restr[#se._in_restr + 1] = zone_id
end
function alife_obj:add_out_restriction(se, zone_id)
	se._out_restr = se._out_restr or {}
	se._out_restr[#se._out_restr + 1] = zone_id
end

function alife_obj:has_info(id, info) return db.actor and db.actor:has_info(info) or false end
function alife_obj:level_name(id) return mock.level_names[id] or "l01_escape" end
function alife_obj:actor() return mock.actor_se end
function alife_obj:switch_distance() return 150 end
function alife() return alife_obj end

mock.level_names = { [1] = "l01_escape", [2] = "l02_garbage" }
mock.level_name = "l01_escape"

level = {
	name = function() return mock.level_name end,
	-- a navmesh that snaps to a one metre grid, so scatter tests can see real,
	-- distinct, reproducible points instead of one magic number
	vertex_id = function(pos)
		mock.vertex_pos = mock.vertex_pos or {}
		local x, z = math.floor(pos.x + 0.5), math.floor(pos.z + 0.5)
		local id = (x + 4096) * 8192 + (z + 4096)
		mock.vertex_pos[id] = vector():set(x, pos.y, z)
		return id
	end,
	valid_vertex_id = function(id) return id ~= nil and id > 0 end,
	vertex_position = function(id)
		return (mock.vertex_pos and mock.vertex_pos[id]) or vector():set(0, 0, 0)
	end,
	object_by_id = function(id) return mock.go[id] end,
	map_add_object_spot = function(id, spot, hint) mock.spots[id .. "|" .. spot] = hint or "" end,
	map_add_object_spot_ser = function(id, spot, hint) mock.spots[id .. "|" .. spot] = hint or "" end,
	map_remove_object_spot = function(id, spot) mock.spots[id .. "|" .. spot] = nil end,
	map_has_object_spot = function(id, spot) return mock.spots[id .. "|" .. spot] and 1 or 0 end,
	get_time_hours = function() return mock.time_hours end,
	get_start_time = function() return ctime(0) end,
}

game_graph = function()
	return {
		vertex = function(_, gvid)
			return { level_id = function() return mock.gvid_level and mock.gvid_level[gvid] or 1 end, level_vertex_id = function() return 777 end }
		end,
		valid_vertex_id = function(_, id) return true end,
	}
end

db = { storage = {}, zone_by_name = {}, actor = nil }

function IsStalker(go) return go and go._stalker == true end
function IsMonster(go) return go and go._monster == true end

-- ---------------------------------------------------------------------------- story objects
story_objects = {}
local sor = { story_id = {} }
function sor:register(obj_id, sid)
	if (self.story_id[sid] and self.story_id[sid] ~= obj_id) then printf("Multiple objects trying to use same story_id %s", sid) return end
	self.story_id[sid] = obj_id
end
function sor:unregister_by_story_id(sid) self.story_id[sid] = nil end
function sor:unregister_by_id(id) for k, v in pairs(self.story_id) do if (v == id) then self.story_id[k] = nil end end end
function sor:get(sid) if (type(sid) == "number") then return self:get_story_id(sid) end return self.story_id[sid] end
function sor:get_story_id(id) for k, v in pairs(self.story_id) do if (v == id) then return k end end end
function story_objects.get_story_objects_registry() return sor end
function get_story_object_id(sid) return sor:get(sid) end
function get_object_story_id(id) return sor:get_story_id(id) end
function get_story_object(sid) local id = sor:get(sid) return id and (db.storage[id] and db.storage[id].object or mock.go[id]) end
mock.sor = sor

-- ---------------------------------------------------------------------------- misc game modules
function has_alife_info(info) return db.actor and db.actor:has_info(info) or false end

function create_ammo(section, pos, lvid, gvid, pid, num)
	local box = system_ini():r_u32(section, "box_size")
	local t = {}
	while (num > box) do t[#t + 1] = alife():create_ammo(section, pos, lvid, gvid, pid, box) num = num - box end
	t[#t + 1] = alife():create_ammo(section, pos, lvid, gvid, pid, num)
	return t
end

news_manager = {
	send_tip = function(actor, text, timeout, sender, showtime, sender_id)
		mock.news[#mock.news + 1] = { text = text, sender = sender, showtime = showtime, tip = true }
		return true
	end,
	relocate_item = function(actor, kind, section, amount)
		mock.news[#mock.news + 1] = { relocate = kind, section = section, amount = amount }
	end,
	relocate_money = function(actor, kind, amount)
		mock.news[#mock.news + 1] = { relocate_money = kind, amount = amount }
	end,
	send_task = function(actor, kind, tsk)
		mock.news[#mock.news + 1] = { task = kind, id = tsk:get_id(), title = tsk:get_title(), icon = tsk:get_icon_name() }
	end,
}

dialogs = {
	who_is_npc = function(a, b) return a:id() == 0 and b or a end,
	who_is_actor = function(a, b) return a:id() == 0 and a or b end,
	relocate_item_section_to_actor = function(npc, actor, section, amount)
		-- dialogs.script: `amount` whole objects (an ammo object is a full box), news counts rounds
		local a = db.actor
		for _ = 1, amount do alife():create(section, a:position(), a:level_vertex_id(), a:game_vertex_id(), a:id()) end
		if (utils.is_ammo(section)) then amount = amount * system_ini():r_s32(section, "box_size") end
		mock.news[#mock.news + 1] = { relocate = "in", section = section, amount = amount, dialog = true }
	end,
	relocate_item_section_from_actor = function(npc, actor, section, amount)
		local n = amount == "all" and mock.count_items(section) or amount
		for _ = 1, n do local go = actor:object(section) if (go) then mock.remove_item(go) end end
		mock.news[#mock.news + 1] = { relocate = "out", section = section, amount = n, dialog = true }
	end,
	relocate_money_to_actor = function(npc, actor, n) actor:give_money(n) mock.news[#mock.news + 1] = { relocate_money = "in", amount = n, dialog = true } end,
	relocate_money_from_actor = function(npc, actor, n) actor:give_money(-n) mock.news[#mock.news + 1] = { relocate_money = "out", amount = n, dialog = true } end,
}

xr_sound = { set_sound_play = function(id, theme) mock.sounds[#mock.sounds + 1] = theme end }

relation_registry = {
	community_relation = function(a, b) return mock.relations and mock.relations[a .. ">" .. b] or 0 end,
	set_community_relation = function(a, b, v) mock.faction_relations[a .. ">" .. b] = v end,
	change_community_goodwill = function(a, id, d) mock.faction_goodwill[a] = (mock.faction_goodwill[a] or 0) + d end,
	community_goodwill = function(a, id) return mock.faction_goodwill[a] or 0 end,
}
game_relations = {
	FRIENDS = 1000, ENEMIES = -1000,
	set_factions_community = function(f, to, v)
		local num = tonumber(v) or (v == "enemy" and -5000) or (v == "friend" and 5000) or 0
		relation_registry.set_community_relation(f, to, num)
	end,
	change_factions_community_num = function(f, id, d) relation_registry.change_community_goodwill(f, id, d) end,
	set_npcs_relation = function(n1, n2, rel)
		local gw = tonumber(rel) or (rel == "enemy" and -1000) or (rel == "friend" and 1000) or 0
		n1:force_set_goodwill(gw, n2)
	end,
}
inventory_upgrades = { victim_id = nil }

-- Alundaio's deferred release: immediate here (a squad takes its members with it).
safe_release_manager = {
	release = function(se)
		mock.safe_released[#mock.safe_released + 1] = se.id
		if (se._members) then mock.squad_release(se, true) else alife():release(se, true) end
	end,
}

function alife_object(id) return mock.se[id] end

utils = {
	is_ammo = function(section) return system_ini():r_string_ex(section, "class") == "AMMO" end,
	CTime_to_table = function(ct) local Y, M, D, h, m, s, ms = ct:get() return { Y = Y, M = M, D = D, h = h, m = m, s = s, ms = ms } end,
	CTime_from_table = function(t) return ctime(0) end,
}

-- ---------------------------------------------------------------------------- SIMBOARD
mock.smarts = {}
function mock.add_smart(name, x, y, z, gvid, radius)
	local smart = { id = mock.next_id, position = vector():set(x, y, z), m_level_vertex_id = 5, m_game_vertex_id = gvid or 1, arrive_dist = radius or 20, _name = name, online = true }
	function smart:name() return self._name end
	mock.next_id = mock.next_id + 1
	mock.smarts[name] = smart
	mock.se[smart.id] = smart
	return smart
end

-- sim_squad_scripted stand-in: members are server objects (online ones get game objects too),
-- `scripted_target` is a plain field like in the game, deaths go through the same callbacks.
local squad_mt = setmetatable({}, { __index = se_mt })
squad_mt.__index = squad_mt
function squad_mt:squad_members()
	local i, list = 0, self._members
	return function()
		i = i + 1
		local id = list[i]
		if (id) then return { id = id, object = mock.se[id] } end
	end
end
function squad_mt:commander_id() return self._members[1] end
function squad_mt:npc_count() return #self._members end
function squad_mt:set_squad_relation(rel) self._relation = rel mock.squad_relations[self.id] = rel end
function squad_mt:get_script_target()
	if (self.scripted_target == "actor") then return 0 end
	local smart = self.scripted_target and mock.smarts[self.scripted_target]
	return smart and smart.id or nil
end
function squad_mt:remove_squad() mock.squad_release(self, true) end
function squad_mt:_drop_member(id)
	for i, mid in ipairs(self._members) do
		if (mid == id) then table.remove(self._members, i) break end
	end
end

SIMBOARD = { smarts_by_names = mock.smarts, smarts = {}, squads = {} }
function SIMBOARD:create_squad(smart, section)
	if not (system_ini():section_exist(section)) then printf("squad section does not exist: %s", section) return nil end
	local squad = setmetatable({ id = mock.next_id, _section = section, position = smart.position, m_level_vertex_id = smart.m_level_vertex_id, m_game_vertex_id = smart.m_game_vertex_id, online = true, _members = {}, smart_id = smart.id }, squad_mt)
	mock.next_id = mock.next_id + 1
	mock.se[squad.id] = squad
	self.squads[squad.id] = squad
	SendScriptCallback("squad_on_register", squad)		-- on_register runs inside alife():create
	for _ = 1, mock.squad_size do
		local m = new_se(section .. "_npc", smart.position, smart.m_level_vertex_id, smart.m_game_vertex_id)
		m.group_id = squad.id
		m._alive = true
		m._monster = true
		squad._members[#squad._members + 1] = m.id
		if (mock.squads_online) then
			local go = new_go(section .. "_npc", { monster = true, alive = true })
			go._id = m.id
			mock.go[m.id] = go
			db.storage[m.id] = { object = go }
		end
		self:setup_squad_and_group(m)
	end
	mock.created_squads = mock.created_squads or {}
	mock.created_squads[#mock.created_squads + 1] = squad
	return squad
end
function SIMBOARD:remove_squad(squad) mock.squad_release(squad, true) end
function SIMBOARD:setup_squad_and_group(se) mock.setup_calls = mock.setup_calls + 1 end
function SIMBOARD:assign_squad_to_smart(squad, smart_id) squad.smart_id = smart_id end
sim_board = { get_sim_board = function() return SIMBOARD end }

-- Squad object gone: sim_squad_scripted:on_unregister -> squad_on_unregister, members released too.
function mock.squad_release(squad, with_members)
	if not (mock.se[squad.id]) then return end
	SendScriptCallback("squad_on_unregister", squad)
	if (with_members) then
		for _, mid in ipairs(squad._members) do
			mock.se[mid] = nil
			mock.go[mid] = nil
			db.storage[mid] = nil
		end
		squad._members = {}
	end
	mock.se[squad.id] = nil
	SIMBOARD.squads[squad.id] = nil
	sor:unregister_by_id(squad.id)
end

-- se_stalker/se_monster:on_death -> squad:on_npc_death: unregister the member, squad_on_npc_death,
-- an empty squad releases itself (killer = server object, like the engine passes it).
function mock.squad_member_died(squad, member_id, killer)
	local mse = mock.se[member_id]
	if (mse) then mse._alive = false end
	squad:_drop_member(member_id)
	SendScriptCallback("squad_on_npc_death", squad, mse, killer and mock.se[killer:id()] or nil)
	if (#squad._members == 0) then mock.squad_release(squad, false) end
end

-- Online kill of a member: the game object's death callback then the server side.
function mock.kill_member(squad, member_id, killer)
	local go = mock.go[member_id]
	if (go) then go:kill(killer) else mock.squad_member_died(squad, member_id, killer) end
end

-- One member dies while nobody is around: no callback of any kind, the object is simply gone.
function mock.member_die_offline(squad, member_id)
	squad:_drop_member(member_id)
	mock.se[member_id] = nil
	mock.go[member_id] = nil
	db.storage[member_id] = nil
end

-- Offline death: nobody is around, no callbacks at all - the objects simply vanish.
function mock.squad_die_offline(squad)
	for _, mid in ipairs(squad._members) do
		mock.se[mid] = nil
		mock.go[mid] = nil
		db.storage[mid] = nil
	end
	squad._members = {}
	mock.se[squad.id] = nil
	SIMBOARD.squads[squad.id] = nil
end

-- ---------------------------------------------------------------------------- ini files (real catalog.ltx)
local ini_mt = {}
ini_mt.__index = ini_mt
local function parse_ini(text)
	local sections, order = {}, {}
	local cur
	for line in string.gmatch(text .. "\n", "([^\n]*)\n") do
		line = string.gsub(line, "\r$", "")
		local body = string.gsub(line, ";.*$", "")
		body = string.gsub(body, "^%s*(.-)%s*$", "%1")
		local sec = string.match(body, "^%[([^%]]+)%]")
		if (sec) then
			cur = { name = sec, lines = {}, map = {} }
			sections[sec] = cur
			order[#order + 1] = sec
		elseif (cur and body ~= "") then
			local k, v = string.match(body, "^([^=]-)%s*=%s*(.*)$")
			if not (k) then k, v = body, "" end
			cur.lines[#cur.lines + 1] = { k, v }
			cur.map[k] = v
		end
	end
	return { sections = sections, order = order }
end
function ini_mt:section_exist(s) return self.d.sections[s] ~= nil end
function ini_mt:line_exist(s, k) local sec = self.d.sections[s] return sec ~= nil and sec.map[k] ~= nil end
function ini_mt:line_count(s) local sec = self.d.sections[s] return sec and #sec.lines or 0 end
function ini_mt:r_line(s, i, a, b) local sec = self.d.sections[s] local l = sec and sec.lines[i + 1] if not (l) then return false, "", "" end return true, l[1], l[2] end
function ini_mt:r_string(s, k) local sec = self.d.sections[s] return sec and sec.map[k] end
function ini_mt:r_string_ex(s, k) return self:r_string(s, k) end
function ini_mt:r_u32(s, k) return tonumber(self:r_string(s, k)) end
function ini_mt:r_s32(s, k) return tonumber(self:r_string(s, k)) end
function ini_mt:r_bool(s, k) local v = self:r_string(s, k) return v == "true" or v == "on" or v == "1" end
function ini_mt:section_for_each(fn) for _, name in ipairs(self.d.order) do fn(name) end end

local function read_file(path)
	local f = io.open(path, "rb")
	if not (f) then return nil end
	local s = f:read("*a")
	f:close()
	return s
end

function ini_file(rel)
	local path = mock.opts.config_dir .. "\\" .. rel
	local text = read_file(path)
	if not (text) then error("ini_file: cannot open " .. path) end
	return setmetatable({ d = parse_ini(text), path = path }, ini_mt)
end

local SYSTEM_INI = [[
[bread]
inv_name = st_bread
[medkit]
inv_name = st_medkit
[wpn_pm]
inv_name = st_wpn_pm
[ammo_9x18_fmj]
inv_name = st_ammo
class = AMMO
box_size = 30
; ammo whose box_size is present but unusable: create_ammo reads it too, so a giver that scales
; rounds by a defaulted box size still ends up inside a broken loop
[ammo_broken_box]
inv_name = st_ammo
class = AMMO
box_size = unreadable
[space_restrictor]
class = SPC_RS_S
[simulation_boar]
class = ON_OFF_S
; a real army squad, the kind an author picks for "spawn soldiers to kill"
[army_sim_squad_novice]
class = ON_OFF_S
faction = army
npc = sim_default_military_1, sim_default_military_2, sim_default_military_3
]]
local system_ini_obj = setmetatable({ d = parse_ini(SYSTEM_INI) }, ini_mt)
function system_ini() return system_ini_obj end

-- ---------------------------------------------------------------------------- FS
FS = { FS_ListFiles = 1, FS_ListFolders = 2, FS_ClampExt = 4, FS_RootOnly = 8 }
local fs_obj = {}
function fs_obj:update_path(alias, rel)
	if (alias == "$game_config$") then return mock.opts.config_dir .. "\\" .. (rel or "") end
	if (alias == "$app_data_root$") then return mock.opts.appdata_dir .. "\\" .. (rel or "") end
	return (rel or "")
end
function fs_obj:file_list_open(alias, sub, flags)
	local dir = self:update_path(alias, sub)
	local names = mock.dir_files(dir, "*.ltx")
	local list = { n = names }
	function list:Size() return #self.n end
	function list:GetAt(i) return self.n[i + 1] end
	function list:Free() end
	return list
end
function fs_obj:exist(alias, rel) return read_file(self:update_path(alias, rel)) ~= nil end
function getFS() return fs_obj end

-- Recursive listing through `dir /s /b`; returns paths relative to `dir` with backslashes.
-- The first output line is the normalised absolute directory (`cd`), used to cut the prefix.
function mock.dir_files(dir, mask)
	local out = {}
	local p = io.popen('cd /d "' .. dir .. '" 2>nul && cd && dir /s /b /a-d "' .. mask .. '" 2>nul')
	if not (p) then return out end
	local base
	for line in p:lines() do
		line = string.gsub(line, "\r$", "")
		if (line ~= "") then
			if not (base) then
				base = line
			elseif (string.sub(line, 1, #base + 1) == base .. "\\") then
				out[#out + 1] = string.sub(line, #base + 2)
			end
		end
	end
	p:close()
	table.sort(out)
	return out
end

-- ---------------------------------------------------------------------------- callbacks
function RegisterScriptCallback(name, fn)
	mock.callbacks[name] = mock.callbacks[name] or {}
	mock.callbacks[name][fn] = true
end
function UnregisterScriptCallback(name, fn)
	if (mock.callbacks[name]) then mock.callbacks[name][fn] = nil end
end
function SendScriptCallback(name, ...)
	local list = mock.callbacks[name]
	if not (list) then return end
	local fns = {}
	for fn in pairs(list) do fns[#fns + 1] = fn end
	for _, fn in ipairs(fns) do fn(...) end
end
function mock.has_callback(name)
	local list = mock.callbacks[name]
	return list ~= nil and next(list) ~= nil
end

-- ---------------------------------------------------------------------------- xms
local registries = {}
local function make_registry()
	local entries = {}
	return {
		add = function(_, key, value, opts)
			if (entries[key] ~= nil and not (opts and opts.override)) then return false end
			entries[key] = value
			return true
		end,
		get = function(_, key) return entries[key] end,
		remove = function(_, key) entries[key] = nil end,
		all = function(_) return entries end,
	}
end

local function module_by_id(id)
	for _, m in ipairs(mock.opts.modules or {}) do if (m.id == id) then return m end end
	return nil
end

local function build_xms()
	registries = {}
	xms = {
		api = 1,
		nq_api = 1,
		log = function(s) printf("[xms] %s", s) end,
		modules = function()
			local out = {}
			for _, m in ipairs(mock.opts.modules or {}) do
				out[#out + 1] = { id = m.id, name = m.name or m.id, version = "1", ns = m.ns or 1, layer = 1, enabled = m.enabled ~= false }
			end
			return out
		end,
		is_loaded = function(id) local m = module_by_id(id) return m ~= nil and m.enabled ~= false end,
		story_id = function(id, n) return n end,
		mode_active = function(id) return false end,
		module_applies = function(id) local m = module_by_id(id) return m ~= nil and m.applies ~= false end,
		graph_vertex = function(lvl, x, y, z) return 1 end,
		save_data = function(id, s)
			mock.save_calls = mock.save_calls + 1
			mock.blobs[id] = s
			return true
		end,
		load_data = function(id) return mock.blobs[id] end,
		list_files = function(id, subdir, mask, recursive)
			local m = module_by_id(id)
			if not (m) then return nil end
			local files = mock.dir_files(m.root, mask or "*.nqasset")
			local out = {}
			for _, f in ipairs(files) do
				if not (mock.deleted[id .. "/" .. f]) then out[#out + 1] = f end
			end
			for key in pairs(mock.overrides) do
				local mid, rel = string.match(key, "^(.-)/(.*)$")
				if (mid == id) then
					local seen = false
					for _, f in ipairs(out) do if (f == rel) then seen = true end end
					if not (seen) then out[#out + 1] = rel end
				end
			end
			table.sort(out)
			return out
		end,
		read_file = function(id, rel)
			local key = id .. "/" .. rel
			if (mock.overrides[key]) then return mock.overrides[key] end
			local m = module_by_id(id)
			if not (m) then return nil end
			return read_file(m.root .. "\\" .. rel)
		end,
		-- CPhraseDialog virtual registry (process-wide, like the engine's static map)
		dialog_register = function(id, fn)
			if not (id and id ~= "" and fn and fn ~= "") then return false end
			mock.dialogs_registered[id] = fn
			return true
		end,
		dialog_unregister = function(id) mock.dialogs_registered[id] = nil end,
		dialog_invalidate = function(id)
			if not (id and id ~= "") then return false end
			if (mock.talking) then return false end	-- an open talk holds CPhrase pointers
			mock.dialog_shared[id] = nil
			mock.dialog_invalidated[id] = (mock.dialog_invalidated[id] or 0) + 1
			return true
		end,
		registry = {
			get = function(name)
				if not (registries[name]) then registries[name] = make_registry() end
				return registries[name]
			end,
		},
	}
	if (mock.list_files_missing) then xms.list_files = nil xms.read_file = nil xms.nq_api = nil end
end

-- ---------------------------------------------------------------------------- script namespaces
local SCRIPT_NAMES = { xms_nq = true, xms_nq_util = true, xms_nq_load = true, xms_nq_kinds = true, xms_nq_console = true, xms_nq_dialog = true, xms_nq_task = true, xms_nq_world = true }
local loading = {}

local function load_script(name)
	if (loading[name]) then return nil end
	local path = mock.opts.scripts_dir .. "\\" .. name .. ".script"
	local chunk, err = loadfile(path)
	if not (chunk) then error("cannot load " .. path .. ": " .. tostring(err)) end
	local env = setmetatable({}, { __index = _G })
	setfenv(chunk, env)
	loading[name] = true
	rawset(_G, name, env)	-- visible during its own load (like the engine)
	chunk()
	loading[name] = nil
	return env
end

local function install_autoload()
	setmetatable(_G, {
		__index = function(t, k)
			if (SCRIPT_NAMES[k]) then return load_script(k) end
			return nil
		end,
	})
end

-- ---------------------------------------------------------------------------- CPhraseDialog recorder
-- The engine calls the registered init function with a CPhraseDialog; only AddPhrase is exposed to
-- Lua. Recorded graph: phrases[id] = { id, text, gw, ord, script = { pre = {}, act = {}, text } },
-- edges[prev] = { id, ... } in insertion order.
local fake_script_mt = {}
fake_script_mt.__index = fake_script_mt
function fake_script_mt:AddPrecondition(name) table.insert(self.ph.script.pre, name) end
function fake_script_mt:AddAction(name) table.insert(self.ph.script.act, name) end
function fake_script_mt:SetScriptText(name) self.ph.script.text = name end
function fake_script_mt:AddHasInfo(s) end
function fake_script_mt:AddDontHasInfo(s) end
function fake_script_mt:AddGiveInfo(s) end
function fake_script_mt:AddDisableInfo(s) end

local fake_phrase_mt = {}
fake_phrase_mt.__index = fake_phrase_mt
function fake_phrase_mt:GetPhraseScript() return setmetatable({ ph = self.ph }, fake_script_mt) end

local fake_dialog_mt = {}
fake_dialog_mt.__index = fake_dialog_mt
-- CPhraseDialog::AddPhrase: a new id creates the vertex and returns it, a repeated id only adds the
-- edge (nil back; a different text is reported like the non-MASTER_GOLD engine does).
function fake_dialog_mt:AddPhrase(text, id, prev, gw)
	assert(type(text) == "string" and type(id) == "string" and type(prev) == "string" and type(gw) == "number", "AddPhrase(text, id, prev, goodwill) types")
	local g = self.g
	local ph = g.phrases[id]
	local created = false
	if not (ph) then
		ph = { id = id, text = text, gw = gw, ord = #g.order + 1, script = { pre = {}, act = {}, text = nil } }
		g.phrases[id] = ph
		g.order[#g.order + 1] = id
		created = true
	elseif (ph.text ~= text) then
		printf("~ Trying to add phrase[%s] with ID[%s], but the ID is already used by phrase[%s]", text, id, ph.text)
	end
	if (prev ~= "") then
		assert(g.phrases[prev], "AddPhrase: previous phrase " .. prev .. " does not exist")
		g.edges[prev] = g.edges[prev] or {}
		table.insert(g.edges[prev], id)
	end
	if (created) then return setmetatable({ ph = ph }, fake_phrase_mt) end
	return nil
end

-- "module.func" -> function, resolved like CScriptEngine::functor (split at the last dot).
local function functor(name)
	local ns, fn = string.match(name, "^(.-)%.([^.]+)$")
	local mod = ns and _G[ns]
	local f = mod and mod[fn]
	assert(type(f) == "function", "functor '" .. tostring(name) .. "' not found")
	return f
end
mock.functor = functor

-- CPhraseDialog::Load of a virtual dialog: shared data is built once per id per process by the
-- registered init function; init failures are swallowed by the engine, a missing root gets a stub.
function mock.dialog_load(id)
	local g = mock.dialog_shared[id]
	if (g) then return g end
	local fn = mock.dialogs_registered[id]
	assert(fn, "dialog " .. tostring(id) .. " is not registered")
	g = { id = id, phrases = {}, edges = {}, order = {} }
	local d = setmetatable({ g = g }, fake_dialog_mt)
	local ok, e = pcall(function() functor(fn)(d) end)
	if not (ok) then printf("! XMS: nq dialog [%s]: init function [%s] failed: %s", id, fn, tostring(e)) end
	if not (g.phrases["0"]) then d:AddPhrase("...", "0", "", -10000) end
	mock.dialog_shared[id] = g
	mock.dialog_loads = mock.dialog_loads + 1
	return g
end

-- ---------------------------------------------------------------------------- talk session
-- Mirrors CUITalkWnd / CPhraseDialog::SayPhrase / CAI_PhraseDialogManager::AnswerPhrase:
--   * Action(speaker, listener, dialog_id, phrase_id) is wrapped (errors swallowed);
--   * Precondition(listener, speaker, dialog_id, phrase_id, next_id) is NOT wrapped: an error is a
--     Lua error in the engine, so it propagates here too;
--   * a non-leaf phrase whose successors are all filtered asserts (R_ASSERT2);
--   * the NPC answers at once with the highest-goodwill available phrase (ties: declaration order);
--   * script texts get (first speaker, second speaker, dialog_id, phrase_id).
local function talk_phrase_text(t, pid)
	local ph = t.graph.phrases[pid]
	if (ph.script.text) then return functor(ph.script.text)(t.first, t.second, t.id, pid) end
	return ph.text
end

local function talk_say(t, phrase_id)
	local g = t.graph
	local ph = g.phrases[phrase_id]
	assert(ph, "unknown phrase " .. tostring(phrase_id))
	local speaker = t.turn
	local listener = (speaker == db.actor) and t.npc or db.actor
	t.log[#t.log + 1] = { who = (speaker == db.actor) and "actor" or "npc", id = phrase_id, text = talk_phrase_text(t, phrase_id) }
	for _, a in ipairs(ph.script.act) do
		local ok, e = pcall(functor(a), speaker, listener, t.id, phrase_id)
		if not (ok) then printf("! [engine] action %s failed: %s", a, tostring(e)) end
	end
	local avail = {}
	local edges = g.edges[phrase_id] or {}
	for i, nid in ipairs(edges) do
		local np = g.phrases[nid]
		local ok = true
		for _, p in ipairs(np.script.pre) do
			local r = functor(p)(listener, speaker, t.id, phrase_id, nid)
			if not (r) then ok = false break end
		end
		if (ok) then avail[#avail + 1] = { ph = np, ord = i } end
	end
	if (#edges > 0) then
		assert(#avail > 0, "R_ASSERT2: No available phrase to say, dialog[" .. t.id .. "] after " .. phrase_id)
		table.sort(avail, function(a, b)
			if (a.ph.gw ~= b.ph.gw) then return a.ph.gw > b.ph.gw end
			return a.ord < b.ord
		end)
	end
	t.turn = listener
	t.avail = {}
	for _, e in ipairs(avail) do t.avail[#t.avail + 1] = e.ph end
	if (#edges == 0) then
		t.current = nil		-- finished -> topic mode
		t.avail = {}
		return
	end
	if (t.turn == t.npc) then talk_say(t, t.avail[1].id) end
end

-- Opens the talk window with an NPC. A registered start dialog of the NPC runs first (NPC speaks
-- the root), like CUITalkWnd::InitOthersStartDialog.
function mock.talk_open(npc)
	if (mock.talk) then mock.talk_close() end
	mock.talking = true
	local t = { npc = npc, current = nil, avail = {}, log = {}, opened = {} }
	mock.talk = t
	local sd = npc:get_start_dialog()
	if (sd and mock.dialogs_registered[sd]) then
		t.id = sd
		t.graph = mock.dialog_load(sd)
		t.first, t.second, t.turn, t.current = npc, db.actor, npc, sd
		t.opened[#t.opened + 1] = sd
		talk_say(t, "0")
	end
	return t
end

-- Topic mode: the actor's UpdateAvailableDialogs -> xms.dialogs_for; every listed dialog is loaded
-- and its caption read without speakers (a script text on the root would dereference NULL).
function mock.talk_topics()
	local t = assert(mock.talk, "no talk open")
	assert(type(xms.dialogs_for) == "function", "xms.dialogs_for is not set")
	local ids = xms.dialogs_for(t.npc, db.actor)
	local captions = {}
	for _, id in ipairs(ids) do
		local g = mock.dialog_load(id)
		assert(not g.phrases["0"].script.text, "SetScriptText on root '0' of " .. id .. " would crash the topic list")
		captions[#captions + 1] = g.phrases["0"].text
	end
	return ids, captions
end

-- The actor picks a topic: InitDialog(actor -> npc), the actor says the root.
function mock.talk_start(dialog_id)
	local t = assert(mock.talk, "no talk open")
	assert(t.current == nil, "a dialog is already running")
	local ids = xms.dialogs_for(t.npc, db.actor)
	local listed = false
	for _, id in ipairs(ids) do if (id == dialog_id) then listed = true end end
	assert(listed, "dialog " .. dialog_id .. " is not offered to the actor")
	t.id = dialog_id
	t.graph = mock.dialog_load(dialog_id)
	t.first, t.second, t.turn, t.current = db.actor, t.npc, db.actor, dialog_id
	t.opened[#t.opened + 1] = dialog_id
	talk_say(t, "0")
	return t
end

-- Actor picks one of the offered phrases.
function mock.talk_say(phrase_id)
	local t = assert(mock.talk, "no talk open")
	assert(t.current, "no dialog running (topic mode)")
	assert(t.turn == db.actor, "it is the NPC's turn")
	local found = false
	for _, ph in ipairs(t.avail) do if (ph.id == phrase_id) then found = true end end
	assert(found, "phrase " .. phrase_id .. " is not offered")
	talk_say(t, phrase_id)
end

function mock.talk_options()
	local t = assert(mock.talk, "no talk open")
	local ids = {}
	for _, ph in ipairs(t.avail) do ids[#ids + 1] = ph.id end
	return ids
end

function mock.talk_option_text(phrase_id)
	local t = assert(mock.talk, "no talk open")
	return talk_phrase_text(t, phrase_id)
end

function mock.talk_close()
	local t = mock.talk
	if not (t) then return end
	mock.talk = nil
	mock.talking = false
	SendScriptCallback("actor_on_leave_dialog", t.npc:id())
end

-- ---------------------------------------------------------------------------- lifecycle
function mock.setup(opts)
	mock.opts = opts
	mock.verbose = opts.verbose
	mock.actor_se = new_se("actor", vector():set(0, 0, 0), 1, 1, nil, 0)
	db.actor = make_actor()
	db.storage[0] = { object = db.actor }
	install_autoload()
	build_xms()
	math.randomseed(1)
end

-- Brand new game (new process): empty blobs, empty world, empty dialog registry, fresh Lua state.
function mock.fresh()
	mock.blobs = {}
	mock.save_calls = 0
	mock.news = {}
	mock.sounds = {}
	mock.spots = {}
	mock.se = {}
	mock.go = {}
	mock.created = {}
	mock.released = {}
	mock.overrides = {}
	mock.deleted = {}
	mock.real_ms = 1000
	mock.game_secs = 0
	mock.next_id = 100
	mock.level_name = "l01_escape"
	mock.talking = false
	mock.talk = nil
	mock.dialogs_registered = {}
	mock.dialog_shared = {}
	mock.dialog_invalidated = {}
	mock.dialog_loads = 0
	mock.forced_talks = {}
	mock.squad_relations = {}
	mock.faction_relations = {}
	mock.faction_goodwill = {}
	mock.safe_released = {}
	mock.created_squads = {}
	mock.task_events = {}
	mock.setup_calls = 0
	SIMBOARD.squads = {}
	for k in pairs(sor.story_id) do sor.story_id[k] = nil end
	mock.actor_se = new_se("actor", vector():set(0, 0, 0), 1, 1, nil, 0)
	db.actor = make_actor()
	db.storage = { [0] = { object = db.actor } }
	db.zone_by_name = {}
	for name, smart in pairs(mock.smarts) do mock.se[smart.id] = smart end
	mock.rebuild()
end

-- New Lua state (load game / level change): script tables, registries and callbacks are gone; the
-- mock world, the actor's task registry and the engine-side dialog data stay.
function mock.rebuild()
	for name in pairs(SCRIPT_NAMES) do rawset(_G, name, nil) end
	mock.callbacks = {}
	mock.talk = nil
	mock.talking = false
	build_xms()
	-- map spots are client-side and vanish with the level too
	mock.spots = {}
end

-- Engine start: axr_main calls every script's on_game_start, then the first actor update.
-- The runtime defers its init one update further (the mode heal rides first_update), so a
-- plain update follows - test code keeps calling first_update() and is initialized after it.
function mock.first_update()
	local nq = xms_nq
	if (nq and nq.on_game_start) then nq.on_game_start() end
	SendScriptCallback("actor_on_first_update", { }, 0)
	SendScriptCallback("actor_on_update", { }, 0)
end

function mock.tick(ms)
	ms = ms or 100
	mock.real_ms = mock.real_ms + ms
	SendScriptCallback("actor_on_update", { }, ms)
end

-- Several ticks; each is at least a poll interval so waits progress.
function mock.ticks(n, ms)
	for _ = 1, n do mock.tick(ms or 260) end
end

function mock.advance_game(secs)
	mock.game_secs = mock.game_secs + secs
end

function mock.move_actor(x, y, z)
	db.actor._pos = vector():set(x, y, z)
end

function mock.news_has(pattern)
	for _, n in ipairs(mock.news) do
		if (n.text and string.find(n.text, pattern, 1, true)) then return true end
	end
	return false
end

function mock.news_count(pattern)
	local c = 0
	for _, n in ipairs(mock.news) do
		if (n.text and string.find(n.text, pattern, 1, true)) then c = c + 1 end
	end
	return c
end

function mock.relocated(kind, section)
	for _, n in ipairs(mock.news) do
		if (n.relocate == kind and n.section == section) then return n.amount end
	end
	return nil
end

-- cp1251 helper for expectations written in UTF-8 inside the test file
function mock.cp(s) return (xms_nq_util.to_cp1251(s)) end

return mock
