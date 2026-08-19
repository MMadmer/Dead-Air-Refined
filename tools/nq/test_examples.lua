-- NQ runtime headless tests (test tooling, not shipped). Run:
--   python tools\nq\run_lua.py tools\nq\test_examples.lua [-v]
-- Drives the three reference quests of the editor repo (docs\nq\examples) plus the broken
-- fixtures through the mock engine and asserts the state machine, persistence and validation.

local HERE = string.match(arg[0], "^(.*)[\\/][^\\/]+$") or "."
local REPO = HERE .. "\\..\\.."
local SCRIPTS = REPO .. "\\packaging\\dead-air-x64\\compatibility\\gamedata\\scripts"
local CONFIG = REPO .. "\\packaging\\dead-air-x64\\compatibility\\gamedata\\configs"
local EXAMPLES = os.getenv("NQ_EXAMPLES") or "D:\\Games\\XFined-Editor\\docs\\nq\\examples"
local FIXTURES = HERE .. "\\fixtures"
local verbose = arg[1] == "-v"

local mock = dofile(HERE .. "\\mock_engine.lua")

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

local function section(name)
	io.write("== ", name, "\n")
end

local function fail_dump()
	io.write("---- last log lines:\n")
	local from = math.max(1, #mock.log - 40)
	for i = from, #mock.log do io.write(mock.log[i], "\n") end
end

local MODS = {
	{ id = "mod_a", root = EXAMPLES },
	{ id = "mod_b", root = FIXTURES .. "\\mod_b" },
}

local wolf					-- Wolf's game object (story id esc_2_12_stalker_wolf), created by setup()

local function setup(opts)
	opts = opts or {}
	mock.setup({
		scripts_dir = SCRIPTS, config_dir = CONFIG, appdata_dir = HERE,
		modules = opts.modules or MODS, verbose = verbose,
	})
	mock.fresh()
	mock.add_smart("esc_smart_terrain_2_12", 100, 0, 100, 1, 20)
	mock.add_smart("esc_smart_terrain_3_16", -200, 0, 50, 1, 20)
	wolf = mock.add_npc("esc_2_12_stalker_wolf", "esc_2_12_stalker_wolf", { community = "stalker", name = "Wolf", profile = "esc_wolf" })
end

local UID_A, UID_B, UID_C = "mod_a.linear_fetch", "mod_a.parallel_triggers", "mod_a.dialog_branching"
local D_MEET, D_REPORT = "nq." .. UID_C .. ".meet", "nq." .. UID_C .. ".report"
local T_BREAD = "nq." .. UID_A .. ".bring_bread"

local function has(list, v)
	for _, x in ipairs(list or {}) do if (x == v) then return true end end
	return false
end

local function task_news(kind, id)
	for _, n in ipairs(mock.news) do
		if (n.task == kind and n.id == id) then return n end
	end
	return nil
end

local function task_news_count(kind, id)
	local n = 0
	for _, news in ipairs(mock.news) do
		if (news.task == kind and news.id == id) then n = n + 1 end
	end
	return n
end

-- ============================================================================ (a) linear_fetch
section("(a) linear_fetch: start -> fetch -> reward -> end")
setup()
mock.first_update()
local core = xms_nq
check(core.is_ready(), "runtime initialised")
local Q, order, invalid = core.quests()
check(Q[UID_A] and Q[UID_B] and Q[UID_C], "three example quests loaded")
check(#invalid == 5, "five invalid fixtures (" .. #invalid .. ")")
check(Q["mod_b.ok_manual"] ~= nil, "valid quest next to broken ones still loads")
check(mock.has_callback("actor_on_update"), "callbacks registered when quests exist")
local qs = core.quest_state(UID_A)
check(qs and qs.status == "active", "linear_fetch active")
check(qs.tokens.fetch ~= nil and qs.tokens.fetch.b == true, "token waits on fetch")
check(qs.done.start == "trigger.start" and qs.done.intro == nil, "trigger.start done marked with its kind, flow.step passed")
check(mock.news_has(mock.cp("Новичкам на Кордоне нужен хлеб")), "intro news.tip sent (cp1251)")
-- task.give: CGameTask in the actor registry, runtime status, PDA news
local tb = mock.task_by_id(T_BREAD)
check(tb ~= nil and tb:get_state() == task.in_progress, "task.give created the PDA task (in progress)")
check(tb and tb:get_title() == mock.cp("Хлеб для новичков") and tb:get_type() == task.additional, "task title (cp1251) and type")
check(tb and tb:get_icon_name() == "ui_pda2_mtask_overlay", "default task icon")
check(qs.tasks.bring_bread == "active", "qs.tasks = active")
check(task_news_count("new", T_BREAD) == 1, "task.give emits one new-task notification")
check(mock.count_items("bread") == 0, "no bread yet")
mock.add_item("bread", true)
mock.ticks(2)
qs = core.quest_state(UID_A)
check(qs.tokens.fetch ~= nil, "one bread is not enough")
mock.add_item("bread", true)
mock.ticks(2)
qs = core.quest_state(UID_A)
check(qs.tokens.fetch == nil, "fetch completed after 2 bread")
check(qs.status == "completed", "quest completed by flow.end (" .. tostring(qs.status) .. ")")
check(mock.count_items("bread") == 0, "reward took the bread (item.take)")
check(mock.count_items("medkit") == 1, "reward gave a medkit (item.give)")
check(db.actor:money() == 1500, "money.give +500 (" .. db.actor:money() .. ")")
check(mock.news_has(mock.cp("Держи аптечку")), "reward news.tip sent")
check(mock.relocated("in", "medkit") == 1, "relocate news for medkit")
check(qs.tasks.bring_bread == "completed", "task.complete -> qs.tasks completed")
check(mock.task_by_id(T_BREAD):get_state() == task.completed, "PDA task state completed")
check(task_news_count("complete", T_BREAD) == 1, "task.complete emits one completion notification")
check(mock.save_calls > 0 and mock.blobs["xms.nq"] ~= nil, "state blob staged under xms.nq")
if (failed > 0) then fail_dump() end

-- ============================================================================ (b1) parallel_triggers
section("(b1) parallel_triggers: timer/reach/join, medkit trigger, bread -> kill -> signal, wait.any timeout")
setup()
mock.deleted["mod_a/linear_fetch.nqasset"] = true	-- keep linear_fetch from eating the bread
mock.first_update()
core = xms_nq
qs = core.quest_state(UID_B)
check(qs and qs.status == "active", "parallel_triggers active")
check(qs.tokens.timer and qs.tokens.reach and not qs.tokens.join, "timer and reach wait, join not yet")
local smart = mock.smarts["esc_smart_terrain_2_12"]
check(mock.spots[smart.id .. "|secondary_task_location"] ~= nil, "objective.reach put a map spot on the smart")
-- timer: 30 game minutes
mock.advance_game(20 * 60)
mock.ticks(1)
check(core.quest_state(UID_B).tokens.timer ~= nil, "timer still waiting at 20 min")
mock.advance_game(11 * 60)
mock.ticks(2)
qs = core.quest_state(UID_B)
check(qs.tokens.timer == nil, "timer done after 31 game minutes")
check(qs.joins.join and next(qs.joins.join) ~= nil, "join counted the first arrival")
check(qs.tokens.arrived == nil and not mock.news_has(mock.cp("Обе дороги сошлись")), "join still waiting for reach")
-- reach: walk into the smart radius
mock.move_actor(105, 0, 100)
mock.ticks(2)
qs = core.quest_state(UID_B)
check(qs.tokens.reach == nil, "reach done inside the smart radius")
check(mock.news_has(mock.cp("Обе дороги сошлись")), "join fired -> arrived news")
check(mock.spots[smart.id .. "|secondary_task_location"] == nil, "reach removed its map spot")
check(qs.joins.join == nil, "join counter reset")
-- medkit trigger (event.item_taken, once)
mock.add_item("medkit", true)
mock.ticks(2)
qs = core.quest_state(UID_B)
check(qs.vars.medkit_seen == true, "medkit trigger set var")
check(mock.news_count(mock.cp("Кабаны кусаются")) == 1, "medkit note once")
check(qs.fired.on_medkit == 1, "fired counter = 1")
mock.add_item("medkit", true)
mock.ticks(2)
check(mock.news_count(mock.cp("Кабаны кусаются")) == 1, "non-repeat trigger does not fire twice")
-- bread trigger (has_item >= 3, polled) -> objective.kill{spawn}
mock.add_item("bread") mock.add_item("bread")
mock.ticks(2)
qs = core.quest_state(UID_B)
check(qs.tokens.hunt == nil, "2 bread: on_bread not fired")
mock.add_item("bread")
mock.ticks(2)
qs = core.quest_state(UID_B)
check(qs.tokens.hunt ~= nil and qs.tokens.hunt.b == true, "3 bread: hunt (objective.kill) waiting")
check(qs.status == "active", "quest still active")
local boars = qs.refs.boars and mock.se[qs.refs.boars.id]
check(boars ~= nil and boars._section == "simulation_boar" and boars.scripted_target == "esc_smart_terrain_3_16", "objective.kill spawned the squad on the smart with hold")
check(qs.tokens.hunt.w.id == qs.refs.boars.id and qs.tokens.hunt.w.kind == "squad", "kill token remembers its squad")
check(mock.task_by_id("nq." .. UID_B .. ".hunt") ~= nil, "task.give in on_enter created the PDA task")
-- squad dies offline (no callbacks): the poll fallback finishes the objective
mock.squad_die_offline(boars)
mock.ticks(2)
qs = core.quest_state(UID_B)
check(qs.tokens.hunt == nil and qs.vars.hunted == true, "offline squad death -> hunt done -> hunted")
check(qs.tasks.hunt == "completed", "task.complete after the hunt")
check(db.actor:money() == 1700, "money.give +700")
check(mock.news_has(mock.cp("Сигнал получен")), "signal.send -> event.signal trigger -> note")
check(qs.tokens.race ~= nil, "wait.any waiting")
-- wait.any: 5 real seconds timeout -> flow.branch -> end_good (medkit_seen = true)
mock.ticks(10, 260)
qs = core.quest_state(UID_B)
check(qs.tokens.race ~= nil, "race still waiting at 2.6 s")
mock.ticks(12, 260)
qs = core.quest_state(UID_B)
check(qs.tokens.race == nil, "race timed out")
check(mock.news_has(mock.cp("аптечка не понадобилась")), "branch with_medkit -> end_good")
check(qs.status == "completed", "parallel_triggers completed")
if (failed > 0) then fail_dump() end

-- ============================================================================ (b2) parallel_triggers: item_used path -> else
section("(b2) parallel_triggers: wait.any by event.item_used, flow.branch else")
setup()
mock.deleted["mod_a/linear_fetch.nqasset"] = true
mock.first_update()
core = xms_nq
for _ = 1, 3 do mock.add_item("bread") end
mock.ticks(2)
qs = core.quest_state(UID_B)
check(qs.tokens.hunt ~= nil, "hunt waiting")
core.complete_external(UID_B, "hunt")	-- default pin
mock.ticks(2)
qs = core.quest_state(UID_B)
check(qs.tokens.race ~= nil, "race waiting after default-pin completion")
mock.add_item("medkit")	-- no item_take notification: medkit_seen stays false
mock.use_item("medkit")
mock.ticks(2)
qs = core.quest_state(UID_B)
check(qs.tokens.race == nil, "event.item_used resolved wait.any")
check(qs.vars.medkit_seen == false, "medkit_seen false")
check(qs.status == "completed" and not mock.news_has(mock.cp("аптечка не понадобилась")), "branch else -> end_plain")
if (failed > 0) then fail_dump() end

-- ============================================================================ (c) dialog_branching validation
section("(c) dialog_branching loads with 0 errors")
setup()
mock.first_update()
core = xms_nq
local qc = core.quest_def(UID_C)
check(qc ~= nil and qc.errors == 0, "dialog_branching valid (" .. tostring(qc and qc.errors) .. " errors)")
if (qc) then
	for _, p in ipairs(qc.problems) do io.write("  problem: ", xms_nq_load.problem_line(p), "\n") end
end
qs = core.quest_state(UID_C)
check(qs and qs.tokens.meet ~= nil, "token on the first dialog.topic")
local ol = xms_nq_load.outline(qc)
check(string.find(ol, "meet %(dialog.topic; npc=story:esc_2_12_stalker_wolf%)", 1) ~= nil, "outline shows the topic")
check(string.find(ol, "%[T%] start %(trigger.start%)", 1) ~= nil, "outline marks the trigger")
if (verbose) then io.write(ol, "\n") end
check(mock.dialogs_registered[D_MEET] ~= nil and mock.dialogs_registered[D_REPORT] ~= nil, "extension on_init registered both topic dialogs")
check(string.find(mock.dialogs_registered[D_MEET], "^xms_nq_dialog%.init_%x+$") ~= nil, "init function name is content-addressed (" .. tostring(mock.dialogs_registered[D_MEET]) .. ")")
check(type(mock.functor(mock.dialogs_registered[D_MEET])) == "function", "init function resolvable like the engine does")

-- ============================================================================ (d) save -> load round trip
section("(d) save/load round trip keeps tokens, vars, refs and restages the blob")
setup()
mock.first_update()
core = xms_nq
mock.add_item("bread", true)		-- linear_fetch: 1 of 2
mock.add_item("medkit", true)		-- parallel: medkit trigger fired -> var
mock.advance_game(10 * 60)
mock.ticks(3)
qs = core.quest_state(UID_B)
core.ref_set(qs, "probe", { id = 4242, kind = "object", section = "probe" })
mock.ticks(1)
local before = xms_nq_util.decode(mock.blobs["xms.nq"])
check(before.quests[UID_A].tokens.fetch ~= nil, "blob has the fetch token")
check(before.quests[UID_B].vars.medkit_seen == true, "blob has medkit_seen")
check(before.quests[UID_B].refs.probe.id == 4242, "blob has the ref")
check(before.quests[UID_B].tokens.timer.w.until_g ~= nil, "wait.timer deadline persisted")
local saves_before = mock.save_calls
mock.rebuild()
mock.first_update()
core = xms_nq
check(core.is_ready(), "re-initialised from the blob")
qs = core.quest_state(UID_A)
check(qs and qs.tokens.fetch ~= nil and qs.tokens.fetch.b == true, "fetch token restored")
qs = core.quest_state(UID_B)
check(qs.vars.medkit_seen == true and qs.fired.on_medkit == 1, "vars and fired restored")
check(qs.refs.probe and qs.refs.probe.id == 4242, "refs restored")
check(mock.sor:get("nq." .. UID_B .. ".probe") == 4242, "ref story id re-registered")
check(mock.save_calls > saves_before, "blob restaged after load")
check(mock.spots[smart.id .. "|secondary_task_location"] ~= nil, "reach map spot re-added on load")
-- and the game goes on: second bread completes the fetch
mock.add_item("bread", true)
mock.ticks(2)
check(core.quest_state(UID_A).status == "completed", "linear_fetch completes after load")
-- timer continues from the persisted deadline
mock.advance_game(21 * 60)
mock.ticks(2)
check(core.quest_state(UID_B).tokens.timer == nil, "timer expired after load using the saved deadline")
if (failed > 0) then fail_dump() end

-- ============================================================================ (e) broken assets
section("(e) broken assets: E003 / E007 without touching the others")
setup()
mock.first_update()
core = xms_nq
Q, order, invalid = core.quests()
local codes = {}
for _, q in ipairs(invalid) do
	for _, p in ipairs(q.problems) do codes[q.file .. ":" .. p.code] = true end
end
check(codes["broken_syntax.nqasset:E003"], "syntax error -> E003")
check(codes["code_outside.nqasset:E003"], "code outside strings -> E003")
check(codes["code_local.nqasset:E003"], "a local declaration that the empty environment cannot catch -> E003")
check(codes["bad_pin.nqasset:E007"], "undeclared pin / missing target / edge into trigger -> E007")
check(codes["bad_dialog.nqasset:E010"] and codes["bad_dialog.nqasset:E011"], "broken alternation -> E010, world edge into a phrase -> E011")
check(mock.dialogs_registered["nq.mod_b.bad_dialog.t"] == nil, "invalid quest registers no dialogs")
check(Q["mod_b.ok_manual"] ~= nil and core.quest_status("mod_b.ok_manual") == "inactive", "manual quest loaded and inactive")
check(mock.log_has("mod_b/broken_syntax.nqasset: E003"), "E003 logged with the file name")
-- console: activate the manual quest, custom lua runs
xms_nq_console.exec("activate mod_b.ok_manual")
mock.ticks(2)
qs = core.quest_state("mod_b.ok_manual")
check(qs and qs.status == "failed", "manual quest ran to flow.end{failed} (" .. tostring(qs and qs.status) .. ")")
check(qs and qs.vars.n == 10, "var.add then custom lua doubled n (" .. tostring(qs and qs.vars.n) .. ")")
check(mock.log_has("lua ran, n=10"), "nq.log from custom lua")
xms_nq_console.exec("list")
check(mock.log_has("mod_b/broken_syntax.nqasset INVALID"), "nq list shows INVALID")
xms_nq_console.exec("state mod_a.parallel_triggers")
check(mock.log_has("%[nq%] tokens: "), "nq state prints tokens")
xms_nq_console.exec("validate")
check(mock.log_has("validate: %d+ file%(s%)"), "nq validate summary")
xms_nq_console.exec("dump")
local f = io.open(HERE .. "\\nq_report.json", "rb")
check(f ~= nil, "nq dump wrote nq_report.json")
if (f) then local s = f:read("*a") f:close() check(string.find(s, '"uid": "mod_a.linear_fetch"', 1, true) ~= nil, "report lists quests") os.remove(HERE .. "\\nq_report.json") end
xms_nq_console.exec("debug 1")
mock.ticks(1)
xms_nq_console.exec("jump mod_a.linear_fetch reward")
mock.ticks(2)
check(core.quest_state(UID_A).status == "completed", "nq jump to reward -> flow.end")
check(mock.news_has("[nq] " .. UID_A .. ": enter reward"), "debug mode mirrors transitions to PDA news")
if (failed > 0) then fail_dump() end

-- ============================================================================ (f) hash mismatch reconciliation
section("(f) graph changed between sessions: token on a removed node is dropped")
setup()
mock.first_update()
core = xms_nq
qs = core.quest_state(UID_A)
check(qs.tokens.fetch ~= nil, "fetch token before the change")
local original = xms.read_file("mod_a", "linear_fetch.nqasset")
local changed = string.gsub(original, '"fetch"', '"fetch2"')	-- node id and the edge into it
check(changed ~= original, "fixture patched")
mock.overrides["mod_a/linear_fetch.nqasset"] = changed
mock.rebuild()
mock.first_update()
core = xms_nq
qs = core.quest_state(UID_A)
check(qs ~= nil and qs.tokens.fetch == nil, "stale token dropped")
check(mock.log_has("token on fetch dropped"), "drop logged")
check(qs.hash == core.quest_def(UID_A).hash, "hash updated")
check(qs.status == "completed", "no tokens and no armed triggers -> implicit completion")
mock.overrides = {}
-- nq reload with an unchanged asset keeps everything
setup()
mock.first_update()
core = xms_nq
mock.add_item("bread", true)
mock.ticks(2)
xms_nq_console.exec("reload")
qs = core.quest_state(UID_A)
check(qs.tokens.fetch ~= nil and qs.tokens.fetch.b == true, "reload keeps the waiting token")
mock.add_item("bread", true)
mock.ticks(2)
check(core.quest_state(UID_A).status == "completed", "quest continues after reload")
if (failed > 0) then fail_dump() end

-- ============================================================================ (g) zero cost / disabled engine
section("(g) no quests -> no callbacks; missing xms.list_files -> disabled with one line")
setup()
mock.first_update()
check(mock.has_callback("actor_on_update"), "actor_on_update registered with quests")
mock.opts.modules = {}
xms_nq.reload()
check(not mock.has_callback("actor_on_update"), "callbacks removed when reload leaves no quests")
setup({ modules = {} })
mock.first_update()
core = xms_nq
check(core.is_ready(), "ready with zero quests")
check(not mock.has_callback("actor_on_update"), "actor_on_update not registered without quests")
mock.list_files_missing = true
setup()
mock.first_update()
check(not xms_nq.is_ready(), "runtime disabled without xms.list_files")
check(mock.log_count("quest runtime disabled") == 1, "exactly one disabled line")
mock.list_files_missing = false

-- ============================================================================ (h) unit checks
section("(h) unit: cp1251, fnv1a, serializer, format_text")
setup()
local util = xms_nq_util
local s, lost = util.to_cp1251("Привет — «мир» Ё ё № … €")
check(#s == 24 and lost == 0, "cyrillic and typography map to cp1251 (" .. #s .. " bytes, " .. lost .. " lost)")
check(string.byte(s, 1) == 0xCF and string.byte(s, 8) == 0x97 and string.byte(s, 16) == 0xA8 and string.byte(s, 18) == 0xB8 and string.byte(s, 20) == 0xB9 and string.byte(s, 22) == 0x85 and string.byte(s, 24) == 0x88, "cp1251 byte values")
local s2, lost2 = util.to_cp1251("ok ☃")
check(s2 == "ok ?" and lost2 == 1, "unknown char -> ? and counted")
check(util.fnv1a("") == 2166136261, "fnv1a empty")
check(util.fnv1a("a") == 0xE40C292C, "fnv1a 'a'")
check(util.fnv1a("foobar") == 0xBF9CF968, "fnv1a 'foobar'")
local t = { a = 1, b = "x\ny", c = { true, false, 2.5 }, ["k k"] = { z = "q" } }
local rt = util.deserialize(util.serialize(t))
check(rt.a == 1 and rt.b == "x\ny" and rt.c[3] == 2.5 and rt["k k"].z == "q", "serialize/deserialize round trip")
check(util.decode(util.encode(t)).c[1] == true, "encode/decode (marshal path)")
local ctx = { qs = { vars = { name = "Wolf", n = 3 } } }
check(util.format_text(ctx, "hi {var:name} x{var:n} {actor}") == "hi Wolf x3 Strelok", "placeholders substituted")
local g0 = util.game_secs()
check(g0 == 1325376000, "game secs are absolute civil seconds (2012-01-01 = " .. g0 .. ")")
mock.advance_game(90061)
check(util.game_secs() - g0 == 90061, "game secs advance with CTime fields (" .. (util.game_secs() - g0) .. ")")

-- ============================================================================ (i) validation codes
section("(i) validation codes on inline assets")
setup()
mock.first_update()
local loader = xms_nq_load
local function codes_of(src)
	local q = loader.load_asset("mod_x", "inline.nqasset", src)
	local set = {}
	for _, p in ipairs(q.problems) do set[p.code] = (set[p.code] or 0) + 1 end
	return set, q
end
local c
c = codes_of([[return { nq = 1, id = "x", nodes = { { id = "s", kind = "trigger.start", out = { next = "e" } }, { id = "e", kind = "flow.end" } } }, nil, true]])
check(c.E003 == 1, "E003 multiple return values after a nil")
local _, shorthand = codes_of([[return { nq = 1, id = "x", vars = { x = 0 }, nodes = {
	{ id = "s", kind = "trigger.start", cond = { kind = "var", params = { name = "x", value = 0 } }, on_enter = { kind = "var.set", params = { name = "x", value = 1 } }, out = { next = "e" } },
	{ id = "e", kind = "flow.end" },
} }]])
local short_start = shorthand.node_by_id.s
check(shorthand.valid and #short_start.cond == 1 and #short_start.on_enter == 1, "single condition/action shorthand is preserved")
local _, sparse = codes_of([[return { nq = 1, id = "x", vars = { x = 0 }, nodes = {
	[2] = { id = "s", kind = "trigger.start", on_enter = { [3] = { kind = "var.set", params = { name = "x", value = 1 } } }, out = { next = "e" } },
	[7] = { id = "e", kind = "flow.end" },
} }]])
check(sparse.valid and sparse.node_by_id.s and #sparse.node_by_id.s.on_enter == 1, "sparse literal lists match editor canonicalization")
c = codes_of([[return { id = "x", nodes = { { id = "s", kind = "trigger.start" } } }]])
check(c.E001 == 1, "E001 nq missing")
c = codes_of([[return { nq = 1, id = "Bad-Id", nodes = { { id = "s", kind = "trigger.start" } } }]])
check(c.E002 == 1, "E002 quest id pattern")
c = codes_of([[return { nq = 1, id = "x", nodes = { { id = "s", kind = "trigger.start" }, { id = "s", kind = "flow.step" }, { id = "Bad", kind = "flow.step" } } }]])
check(c.E004 == 2, "E004 duplicate / bad node id (" .. tostring(c.E004) .. ")")
c = codes_of([[return { nq = 1, id = "x", nodes = { { id = "s", kind = "trigger.start", out = { next = "a" } }, { id = "a", kind = "no.such", }, { id = "b", kind = "item.give" }, { id = "c", kind = "flow.step", on_enter = { { kind = "var" } }, cond = { { kind = "item.give" } } } } }]])
check(c.E005 == 4, "E005 unknown kind / wrong position x4 (" .. tostring(c.E005) .. ")")
c = codes_of([[return { nq = 1, id = "x", nodes = { { id = "s", kind = "trigger.start", out = { next = "a" } }, { id = "a", kind = "objective.fetch", params = { count = 0 } }, { id = "b", kind = "flow.step", on_enter = { { kind = "item.give", params = { section = 5 } }, { kind = "news.tip", params = { text = "ok", duration = "x" } } } }, { id = "c", kind = "wait.timer" } } }]])
check(c.E006 == 4, "E006 required/type/min x4 (" .. tostring(c.E006) .. ")")
check(c.E022 == 1, "E022 objective.fetch with neither section nor item (" .. tostring(c.E022) .. ")")
c = codes_of([[return { nq = 1, id = "x", nodes = { { id = "a", kind = "flow.step" } } }]])
check(c.E008 == 1 and c.W020 == 1, "E008 no trigger, W020 unreachable")
c = codes_of([[return { nq = 1, id = "x", nodes = { { id = "s", kind = "trigger.start", out = { next = "j" } }, { id = "j", kind = "flow.join" }, { id = "z", kind = "flow.join" } } }]])
check(c.E009 == 1 and c.W021 == 2 and c.W020 == 1, "E009 join without inputs, W021 waiting nodes without outputs")
local duplicate_codes, duplicate_edges = codes_of([[return { nq = 1, id = "x", nodes = { { id = "s", kind = "trigger.start", out = { next = { "e", "e" } } }, { id = "e", kind = "flow.end" } } }]])
check(duplicate_codes.W022 == 1 and #duplicate_edges.node_by_id.s.out.next == 1, "W022 duplicate edge is reported and ignored")
c = codes_of([[return { nq = 1, id = "x", nodes = {
	{ id = "s", kind = "trigger.start", out = { next = "t" } },
	{ id = "t", kind = "dialog.topic", params = { npc = { story = "n" }, text = "hi" }, out = { next = "p1" } },
	{ id = "p1", kind = "dialog.actor_phrase", params = { text = "wrong side" }, out = { next = "p2" } },
	{ id = "p2", kind = "dialog.npc_phrase", params = { text = "x" }, cond = { { kind = "event.signal", params = { name = "s" } } } },
	{ id = "p3", kind = "dialog.npc_phrase", params = { text = "orphan" } },
	{ id = "w", kind = "flow.step", out = { next = "p3" } },
	{ id = "e", kind = "flow.end" },
} }]])
check(c.E010 == 3, "E010 alternation x2 + unreachable phrase (" .. tostring(c.E010) .. ")")
check(c.E011 == 1, "E011 phrase entered from the world")
check(c.E020 == 1, "E020 event cond in a phrase")
check(c.W011 == 1, "W011 non-leaf phrase without unconditional continuation")
c = codes_of([[return { nq = 1, id = "x", nodes = {
	{ id = "s", kind = "trigger.start", out = { next = "b" } },
	{ id = "b", kind = "flow.branch", params = { cases = { { name = "a", cond = { { kind = "event.item_taken" } } }, { name = "a", cond = {} } } }, out = { a = "r" } },
	{ id = "r", kind = "flow.random", params = { cases = { { name = "q", weight = 0 } } } },
	{ id = "w", kind = "wait.any", params = { cases = {} } },
	{ id = "e", kind = "flow.end" },
} }]])
check(c.E020 == 1, "E020 event in flow.branch cases")
check(c.E021 == 3, "E021 duplicate case, weight <= 0, empty cases (" .. tostring(c.E021) .. ")")
c = codes_of([[return { nq = 1, id = "x", vars = { a = 1 }, tasks = { t = { title = "t" } }, nodes = {
	{ id = "s", kind = "trigger.start", out = { next = "b" } },
	{ id = "b", kind = "flow.step", on_enter = {
		{ kind = "var.set", params = { name = "zz", value = 1 } },
		{ kind = "task.give", params = { task = "nope" } },
		{ kind = "map.spot", params = { target = { ref = "ghost" } } },
		{ kind = "lua", params = { code = "this is not lua" } },
	}, cond = { { kind = "node_done", params = { node = "missing" } } }, out = { next = "e" } },
	{ id = "e", kind = "flow.end" },
} }]])
check(c.E030 == 4, "E030 var/task/node/ref references (" .. tostring(c.E030) .. ")")
check(c.W030 == nil, "W030 is not the game's code for an uncreated ref any more")
check(c.E050 == 1, "E050 lua syntax error")
c = codes_of([[return { nq = 1, id = "x", nodes = {
	{ id = "s", kind = "trigger.start", out = { next = "b" } },
	{ id = "b", kind = "flow.step", on_enter = { { kind = "spawn.squad", params = { section = "sq", smart = "sm", hold = false } }, { kind = "news.tip", params = { text = "☃ snow" } } } },
} }]])
check(c.W031 == 1 and c.W040 == 1 and c.W070 == 1, "W031 hold=false, W040 char outside cp1251, W070 no flow.end")
local _, qq = codes_of([[return { nq = 1, id = "x", title = { eng = "Eng", rus = "Рус" }, nodes = { { id = "s", kind = "trigger.start", out = { next = "e" } }, { id = "e", kind = "flow.end", once = "yes" } } }]])
check(qq.title == mock.cp("Рус") and qq.errors == 1, "language pick rus + E006 once type")
c = codes_of([[return { nq = 1, id = "x", nodes = { { id = "s", kind = "trigger.start", out = { next = "t" } }, { id = "t", kind = "dialog.topic", params = { npc = { story = "n" }, text = "hi" }, once = false } } }]])
check(c.W012 == 1 and c.W013 == 1, "W012 topic without phrases, W013 topic once=false without conds")

-- ============================================================================ (j) restart, repeat+cooldown, level_entered, quest.activate, random, real timer
section("(j) flow.end restart, trigger.when repeat/cooldown, level_entered, quest.activate, flow.random, wait.timer real")
setup()
mock.overrides["mod_a/loop.nqasset"] = [[return { nq = 1, id = "loop", vars = { n = 0 }, nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "tick" } },
	{ id = "tick", kind = "flow.step", on_enter = { { kind = "var.add", params = { name = "n", delta = 1 } } }, out = { next = "w" } },
	{ id = "w", kind = "wait.when", params = { timeout = { seconds = 1 } }, cond = { { kind = "has_info", params = { info = "never" } } }, out = { done = "fin", timeout = "br" } },
	{ id = "br", kind = "flow.branch", params = { cases = { { name = "again", cond = { { kind = "has_info", params = { info = "stop" }, ["not"] = true } } } } }, out = { again = "fin_restart", ["else"] = "fin" } },
	{ id = "fin_restart", kind = "flow.end", params = { status = "completed", restart = true } },
	{ id = "fin", kind = "flow.end", params = { status = "failed" } },
} }]]
mock.overrides["mod_a/rep.nqasset"] = [[return { nq = 1, id = "rep", vars = { hits = 0 }, nodes = {
	{ id = "t", kind = "trigger.when", params = { ["repeat"] = true, cooldown = { seconds = 1 } }, cond = { { kind = "has_item", params = { section = "bread" } } }, out = { next = "hit" } },
	{ id = "hit", kind = "flow.step", on_enter = { { kind = "var.add", params = { name = "hits", delta = 1 } } } },
} }]]
mock.overrides["mod_a/misc.nqasset"] = [[return { nq = 1, id = "misc", vars = { entered = false, pick = "" }, nodes = {
	{ id = "lvl", kind = "trigger.when", cond = { { kind = "event.level_entered", params = { level = "l01_escape" } } }, out = { next = "mark" } },
	{ id = "mark", kind = "flow.step", on_enter = { { kind = "var.set", params = { name = "entered", value = true } }, { kind = "quest.activate", params = { quest = "manual2" } } }, out = { next = "rnd" } },
	{ id = "rnd", kind = "flow.random", params = { cases = { { name = "a", weight = 1 }, { name = "b", weight = 1 } } }, out = { a = "pa", b = "pb" } },
	{ id = "pa", kind = "flow.step", on_enter = { { kind = "var.set", params = { name = "pick", value = "a" } } }, out = { next = "tm" } },
	{ id = "pb", kind = "flow.step", on_enter = { { kind = "var.set", params = { name = "pick", value = "b" } } }, out = { next = "tm" } },
	{ id = "tm", kind = "wait.timer", params = { duration = { seconds = 2 } }, out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.overrides["mod_a/manual2.nqasset"] = [[return { nq = 1, id = "manual2", activation = "manual", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "s" } },
	{ id = "s", kind = "flow.step", on_enter = { { kind = "info.give", params = { info = "m2_done" } }, { kind = "sound.play", params = { theme = "pda_tips" } } }, out = { next = "e" } },
	{ id = "e", kind = "flow.end" },
} }]]
mock.first_update()
core = xms_nq
local UL, UR, UM, UM2 = "mod_a.loop", "mod_a.rep", "mod_a.misc", "mod_a.manual2"
-- misc (checked first: its real timer is short)
qs = core.quest_state(UM)
check(qs.vars.entered == true, "level_entered fired on first run")
check(qs.vars.pick == "a" or qs.vars.pick == "b", "flow.random picked a case (" .. tostring(qs.vars.pick) .. ")")
check(core.quest_status(UM2) == "completed" and has_alife_info("m2_done"), "quest.activate ran the manual quest to completion")
check(mock.sounds[1] == "pda_tips", "sound.play reached xr_sound")
check(qs.tokens.tm ~= nil, "wait.timer real waiting")
qs = core.quest_state(UL)
check(qs and qs.vars.n == 1 and qs.tokens.w ~= nil, "loop: first pass waits with n=1")
mock.ticks(6)	-- 1.56 s real
qs = core.quest_state(UL)
check(qs.status == "active" and qs.vars.n == 1 and qs.tokens.w ~= nil, "loop: restart reset vars and re-armed trigger.start (n=" .. tostring(qs.vars.n) .. ")")
check(mock.log_has("mod_a.loop: completed %(restart%)"), "restart logged")
db.actor:give_info_portion("stop")
mock.ticks(6)
qs = core.quest_state(UL)
check(qs.status == "failed", "loop: else branch -> flow.end failed (" .. tostring(qs.status) .. ")")
-- repeat + cooldown
qs = core.quest_state(UR)
check(qs.status == "active" and qs.vars.hits == 0, "rep: armed, no bread")
local bread = mock.add_item("bread")
mock.ticks(1)
qs = core.quest_state(UR)
check(qs.vars.hits == 1, "rep: rising edge fired once")
mock.ticks(1)
check(core.quest_state(UR).vars.hits == 1, "rep: level stays true -> no refire")
mock.remove_item(bread)
mock.ticks(1)
bread = mock.add_item("bread")
mock.ticks(1)	-- 0.78 s after the fire: still inside the 1 s cooldown
check(core.quest_state(UR).vars.hits == 1, "rep: second edge inside cooldown waits")
mock.ticks(3)
check(core.quest_state(UR).vars.hits == 2, "rep: fires after the cooldown (hits=" .. core.quest_state(UR).vars.hits .. ")")
check(core.quest_state(UR).status == "active", "rep: repeat trigger keeps the quest active")
check(core.quest_status(UM) == "completed", "wait.timer real seconds elapsed -> misc completed")
if (failed > 0) then fail_dump() end

-- ============================================================================ (k) error policy
section("(k) error policy: action errors skip, cond errors read false, begin errors retry after 5 s, cancel on flow.end")
setup()
mock.overrides["mod_a/errs.nqasset"] = [[return { nq = 1, id = "errs", vars = { after = false, branch = "" }, nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "s" } },
	{ id = "s", kind = "flow.step", on_enter = {
		{ kind = "lua", params = { code = "error('action boom')" } },
		{ kind = "var.set", params = { name = "after", value = true } },
	}, out = { next = "b" } },
	{ id = "b", kind = "flow.branch", params = { cases = { { name = "bad", cond = { { kind = "lua", params = { code = "return nil + 1" } } } } } },
	  out = { bad = "pbad", ["else"] = "pelse" } },
	-- pbad is the branch NOT taken: it declares the ref (so the graph validates) and never runs,
	-- which is exactly the "used before its creator on this path" case the retry exists for
	{ id = "pbad", kind = "flow.step", on_enter = {
		{ kind = "var.set", params = { name = "branch", value = "bad" } },
		{ kind = "spawn.object", params = { section = "ghost_npc", place = { level = "l01_escape", pos = { 0, 0, 0 } }, ref = "ghost" } },
	} },
	{ id = "pelse", kind = "flow.step", on_enter = { { kind = "var.set", params = { name = "branch", value = "else" } } }, out = { next = "kill" } },
	{ id = "kill", kind = "objective.kill", params = { target = { ref = "ghost" } }, out = { done = "e" } },
	{ id = "e", kind = "flow.end" },
} }]]
mock.first_update()
core = xms_nq
local UE = "mod_a.errs"
qs = core.quest_state(UE)
check(qs.vars.after == true, "action error skipped, following action ran")
check(mock.log_has("errs/s: enter#0 lua: .*action boom"), "action error logged with slot and kind")
check(qs.vars.branch == "else", "cond error reads as false -> else branch")
check(qs.tokens.kill ~= nil and qs.tokens.kill.b == nil, "begin error (ref not created yet) keeps the token, not begun")
check(qs.errors.kill ~= nil and string.find(qs.errors.kill, "ref 'ghost' is not created yet", 1, true) ~= nil, "errors[node] recorded")
mock.ticks(4)	-- ~1 s: no retry yet
qs = core.quest_state(UE)
check(qs.tokens.kill.b == nil, "no retry inside 5 s")
local ghost = mock.add_npc("ghost_npc", nil, { name = "Ghost" })
core.ref_set(qs, "ghost", { id = ghost:id(), kind = "npc", section = "ghost_npc" })
mock.ticks(20)	-- > 5 s
qs = core.quest_state(UE)
check(qs.tokens.kill.b == true and qs.errors.kill == nil, "begin retried after 5 s and succeeded once the ref exists")
core.finish_quest(UE, "failed", false)
check(next(core.quest_state(UE).tokens) == nil and core.quest_status(UE) == "failed", "flow.end path cancels waiting tokens")
if (failed > 0) then fail_dump() end

-- ============================================================================ (l) dialog_branching
section("(l) dialog_branching: dialogs_for, phrase graph, talk -> accept -> kill -> report -> reward -> end")
setup()
mock.first_update()
core = xms_nq
local dlg = xms_nq_dialog
qs = core.quest_state(UID_C)
check(qs and qs.tokens.meet ~= nil and qs.tokens.meet.b == true, "meet topic waits")
-- topic list for Wolf and for a stranger
local other_npc = mock.add_npc("stranger", "esc_stranger", { community = "bandit", name = "Stranger" })
local ids = xms.dialogs_for(wolf, db.actor)
check(#ids == 1 and ids[1] == D_MEET, "dialogs_for(wolf) lists the meet topic only (" .. table.concat(ids, ",") .. ")")
check(#xms.dialogs_for(other_npc, db.actor) == 0, "dialogs_for(stranger) is empty")
check(mock.log_has("dialogs: 2 topic%(s%) registered"), "both topic dialogs registered")
-- the recorded graph
local g = mock.dialog_load(D_MEET)
check(g.phrases["0"] and g.phrases["0"].text == mock.cp("Слышал, у тебя проблемы с кабанами.") and g.phrases["0"].gw == -10000, "root '0' = topic text (cp1251)")
check(g.phrases["0"].script.text == nil and #g.phrases["0"].script.pre == 0 and has(g.phrases["0"].script.act, "xms_nq_dialog.act"), "root: static text, no precondition, action attached")
check(g.edges["0"] and #g.edges["0"] == 1 and g.edges["0"][1] == "wolf_1", "root -> wolf_1")
check(g.phrases.wolf_1.gw == -10000 and g.phrases.wolf_1.text == mock.cp("Лезут из леса каждую ночь. Перебьёшь их — не обижу."), "wolf_1 goodwill -10000, cp1251 text")
check(#g.phrases.wolf_1.script.pre == 0, "unconditional phrase: no precondition attached (structure = availability)")
local kids = g.edges.wolf_1 or {}
check(#kids == 3 and kids[1] == "accept" and kids[2] == "ask_more" and kids[3] == "decline", "wolf_1 -> accept, ask_more, decline in declared order")
check(g.phrases.accept.gw == -10000 and g.phrases.ask_more.gw == -10001 and g.phrases.decline.gw == -10002, "children goodwill -10000 - index")
check(#g.phrases.wolf_rich.script.pre == 1 and g.phrases.wolf_rich.script.pre[1] == "xms_nq_dialog.pre", "conditional phrase has the precondition")
check(#g.phrases.wolf_poor.script.pre == 0, "wolf_poor unconditional")
check(g.edges.ask_more and g.edges.ask_more[1] == "wolf_rich" and g.edges.ask_more[2] == "wolf_poor"
	and g.edges.ask_more[3] == "nq_auto_ask_more" and g.phrases["nq_auto_ask_more"] ~= nil,
	"wolf_rich (cond) + wolf_poor (uncond): the group still gets the hidden fallback leaf")
check(#g.phrases["nq_auto_ask_more"].script.pre == 0 and g.phrases["nq_auto_ask_more"].gw < g.phrases.wolf_poor.gw,
	"the fallback leaf is unconditional and ranks last")
check(g.edges.back and g.edges.back[1] == "wolf_1" and g.phrases.wolf_1.ord < g.phrases.back.ord, "back -> wolf_1 (edge to an existing phrase, nil return handled)")
local nauto = 0
for id in pairs(g.phrases) do if (string.find(id, "^nq_auto_")) then nauto = nauto + 1 end end
check(nauto == 1, "one automatic reply in the reference quest (the ask_more group has a conditional child)")
check(mock.dialog_loads == 1, "graph built once")
-- talk 1: ask twice (rich, then poor after spending), loop back, accept
mock.talk_open(wolf)
local topics = mock.talk_topics()
check(#topics == 1 and topics[1] == D_MEET, "topic mode lists meet")
mock.talk_start(D_MEET)
check(mock.talk.log[1].who == "actor" and mock.talk.log[1].id == "0", "actor says the topic")
check(mock.talk.log[2].who == "npc" and mock.talk.log[2].id == "wolf_1", "NPC answers wolf_1")
local opts = mock.talk_options()
check(#opts == 3 and opts[1] == "accept" and opts[2] == "ask_more" and opts[3] == "decline", "actor options in declared order (" .. table.concat(opts, ",") .. ")")
mock.talk_say("ask_more")
check(mock.talk.log[#mock.talk.log].id == "wolf_rich", "has_money 1000 -> wolf_rich picked first")
check(core.quest_state(UID_C).vars.asked == 1, "ask_more on_exit: asked = 1")
opts = mock.talk_options()
check(#opts == 1 and opts[1] == "back", "only 'back' after wolf_rich")
mock.talk_say("back")
check(mock.talk.log[#mock.talk.log].id == "wolf_1", "back -> wolf_1 again (cycle)")
db.actor._money = 500
mock.talk_say("ask_more")
check(mock.talk.log[#mock.talk.log].id == "wolf_poor", "500 RU -> wolf_rich filtered, wolf_poor said")
check(core.quest_state(UID_C).vars.asked == 2, "asked = 2")
mock.talk_say("back")
mock.talk_say("accept")
qs = core.quest_state(UID_C)
check(qs.vars.agreed == true, "accept on_exit: agreed = true")
check(qs.tasks.kill_boars == "active" and mock.task_by_id("nq." .. UID_C .. ".kill_boars") ~= nil, "accept on_exit: task kill_boars given")
check(mock.talk.current == nil, "accept is a leaf: dialog finished")
check(qs.tokens.meet ~= nil and qs.tokens.kill == nil, "nothing moved yet: transitions wait for the queue")
check(#mock.talk_topics() == 0, "topic pending completion is already hidden from the list")
mock.tick(50)
qs = core.quest_state(UID_C)
check(qs.tokens.meet == nil and qs.done.meet == "dialog.topic", "next frame: meet completed (once -> done)")
check(qs.tokens.kill ~= nil and qs.tokens.kill.b == true, "kill entered from the world exit of accept")
local sq = qs.refs.boars and mock.se[qs.refs.boars.id]
check(sq ~= nil and sq._section == "simulation_boar" and sq.smart_id == mock.smarts["esc_smart_terrain_3_16"].id, "squad spawned at esc_smart_terrain_3_16, ref boars")
check(sq.scripted_target == "esc_smart_terrain_3_16" and qs.refs.boars.hold == true, "hold: scripted_target = smart")
check(mock.sor:get("nq." .. UID_C .. ".boars") == sq.id, "ref story id registered")
check(#mock.talk_topics() == 0, "meet gone from dialogs_for (once)")
mock.talk_close()
-- (n) save -> new Lua state -> load in the middle: dialogs re-registered, hold re-applied, task kept
section("(n) save/rebuild/load mid quest: dialogs re-registered, hold re-applied, tasks kept")
mock.tick(50)
sq.scripted_target = nil
mock.dialogs_registered = {}		-- pretend the process forgot (it does not, but re-registration must be idempotent)
mock.rebuild()
mock.first_update()
core = xms_nq
qs = core.quest_state(UID_C)
check(qs and qs.tokens.kill ~= nil and qs.tokens.kill.b == true and qs.tokens.kill.w.id == sq.id, "kill token restored with its squad id")
check(mock.dialogs_registered[D_MEET] ~= nil and mock.dialogs_registered[D_REPORT] ~= nil, "topic dialogs registered again")
check((mock.dialog_invalidated[D_MEET] or 0) >= 1 and mock.dialog_shared[D_MEET] == nil, "cached graph invalidated on init")
check(sq.scripted_target == "esc_smart_terrain_3_16", "hold re-applied on init")
sq.scripted_target = nil
SendScriptCallback("squad_on_register", sq)
check(sq.scripted_target == "esc_smart_terrain_3_16", "hold re-applied on squad_on_register")
check(mock.task_by_id("nq." .. UID_C .. ".kill_boars") ~= nil and qs.tasks.kill_boars == "active", "task survives the load")
check(#xms.dialogs_for(wolf, db.actor) == 0, "no topic offered while the hunt runs")
-- kill the boars online: squad_npc_death events, last member -> squad_dead
local members = {}
for k in sq:squad_members() do members[#members + 1] = k.id end
check(#members == 2, "two boars")
mock.kill_member(sq, members[1], db.actor)
mock.ticks(1)
qs = core.quest_state(UID_C)
check(qs.tokens.kill ~= nil and qs.tokens.kill.w.hit == true, "one boar down: still waiting, actor kill noted")
mock.kill_member(sq, members[2], db.actor)
mock.ticks(2)
qs = core.quest_state(UID_C)
check(qs.tokens.kill == nil and qs.tokens.report ~= nil, "squad dead -> kill done -> report topic waits")
check(mock.se[sq.id] == nil, "squad object gone")
-- talk 2: report -> reward -> end
mock.talk_open(wolf)
topics = mock.talk_topics()
check(#topics == 1 and topics[1] == D_REPORT, "report topic offered")
local money0 = db.actor:money()
mock.talk_start(D_REPORT)
check(mock.talk.log[#mock.talk.log].id == "wolf_thanks" and mock.talk.current == nil, "NPC says wolf_thanks (leaf), dialog finished")
check(mock.count_items("wpn_pm") == 1 and mock.relocated("in", "wpn_pm") == 1, "item.give through the dialog transfer")
check(db.actor:money() == money0 + 1500, "money.give +1500 in dialog")
check(core.quest_state(UID_C).tasks.kill_boars == "completed" and task_news_count("complete", "nq." .. UID_C .. ".kill_boars") == 1, "dialog task.complete emits one completion notification")
mock.talk_close()
mock.ticks(2)
qs = core.quest_state(UID_C)
check(qs.status == "completed" and next(qs.tokens) == nil, "report done -> finish -> quest completed")
check(#xms.dialogs_for(wolf, db.actor) == 0, "no topics left")
if (failed > 0) then fail_dump() end

-- ============================================================================ (l2) automatic reply + placeholders
section("(l2) automatic reply where every child is conditional; placeholders: static root, dynamic phrases")
setup()
mock.overrides["mod_a/auto.nqasset"] = [[return { nq = 1, id = "auto", vars = { x = 0 }, nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "t" } },
	{ id = "t", kind = "dialog.topic", params = { npc = { story = "esc_2_12_stalker_wolf" }, text = "Тема {var:x}." }, out = { next = "n1" } },
	{ id = "n1", kind = "dialog.npc_phrase", params = { text = "Реплика {var:x}" }, out = { next = { "a_rich", "a_x" } } },
	{ id = "a_rich", kind = "dialog.actor_phrase", params = { text = "rich" }, cond = { { kind = "has_money", params = { amount = 5000 } } }, out = { next = "n_end" } },
	{ id = "a_x", kind = "dialog.actor_phrase", params = { text = "x" }, cond = { { kind = "var", params = { name = "x", op = "eq", value = 1 } } }, out = { next = "n_end" } },
	{ id = "n_end", kind = "dialog.npc_phrase", params = { text = "конец" }, out = { next = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.first_update()
core = xms_nq
local UAUTO, D_T = "mod_a.auto", "nq.mod_a.auto.t"
check(core.quest_def(UAUTO) ~= nil and core.quest_def(UAUTO).warnings >= 1, "W011 reported at load")
mock.talk_open(wolf)
check(has(mock.talk_topics(), D_T), "topic offered")
g = mock.dialog_load(D_T)
check(g.phrases["0"].text == mock.cp("Тема 0.") and g.phrases["0"].script.text == nil, "root text formatted statically at build (x=0)")
check(g.phrases.n1.script.text == "xms_nq_dialog.text", "phrase with a placeholder uses SetScriptText")
check(g.phrases["nq_auto_n1"] ~= nil and g.phrases["nq_auto_n1"].text == mock.cp("Пока.") and g.phrases["nq_auto_n1"].gw == -10002, "automatic actor reply added last")
check(#g.phrases["nq_auto_n1"].script.pre == 0 and #g.phrases["nq_auto_n1"].script.act == 0, "automatic reply: no scripts")
check(g.edges["nq_auto_n1"] == nil, "automatic reply is a leaf")
mock.talk_start(D_T)
check(mock.talk.log[2].id == "n1" and mock.talk.log[2].text == mock.cp("Реплика 0"), "dynamic phrase text formatted on show")
opts = mock.talk_options()
check(#opts == 1 and opts[1] == "nq_auto_n1", "no money, x=0: only the automatic reply survives (assert guard)")
mock.talk_say("nq_auto_n1")
check(mock.talk.current == nil, "automatic reply closes the dialog")
mock.tick(50)
check(core.quest_state(UAUTO).tokens.t ~= nil and has(mock.talk_topics(), D_T), "automatic reply does not pass the topic: still offered")
core.set_var(UAUTO, "x", 1)
mock.talk_start(D_T)
check(mock.talk.log[#mock.talk.log].text == mock.cp("Реплика 1"), "phrase text follows the var")
opts = mock.talk_options()
check(#opts == 2 and opts[1] == "a_x" and opts[2] == "nq_auto_n1", "a_x now available, automatic reply still last")
mock.talk_say("a_x")
check(mock.talk.log[#mock.talk.log].id == "n_end" and mock.talk.current == nil, "n_end leaf said")
mock.talk_close()
mock.ticks(2)
check(core.quest_status(UAUTO) == "completed", "leaf -> topic done -> fin")
-- root text is frozen until the graph is rebuilt: nq reload invalidates
check(mock.dialog_load(D_T).phrases["0"].text == mock.cp("Тема 0."), "root caption still the build-time text")
xms_nq_console.exec("reload")
check(mock.dialog_shared[D_T] == nil, "nq reload dropped the cached graph")
if (failed > 0) then fail_dump() end

-- ============================================================================ (m) initiator = npc
section("(m) initiator=npc: start dialog set while the topic waits, re-applied, restored at the end")
setup()
mock.overrides["mod_a/npc_init.nqasset"] = [[return { nq = 1, id = "npc_init", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "greet" } },
	{ id = "greet", kind = "dialog.topic", params = { npc = { story = "esc_2_12_stalker_wolf" }, text = "Эй, сталкер, подойди.", initiator = "npc" }, out = { next = "a1" } },
	{ id = "a1", kind = "dialog.actor_phrase", params = { text = "Чего тебе?" }, out = { next = "w1" } },
	{ id = "w1", kind = "dialog.npc_phrase", params = { text = "Ничего. Иди." }, out = { next = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.first_update()
core = xms_nq
local UN, D_GREET = "mod_a.npc_init", "nq.mod_a.npc_init.greet"
check(core.quest_state(UN).tokens.greet ~= nil, "greet waits")
check(wolf:get_start_dialog() == nil, "start dialog not touched before the first poll")
mock.ticks(1)
check(wolf:get_start_dialog() == D_GREET, "poll set the start dialog on Wolf")
check(not has(xms.dialogs_for(wolf, db.actor), D_GREET), "npc-initiated topic is not in the actor's list")
wolf:set_start_dialog("hello_dialog")	-- xr_meet interferes
mock.ticks(1)
check(wolf:get_start_dialog() == D_GREET, "start dialog re-applied over xr_meet")
mock.talk_open(wolf)
check(mock.talk.opened[1] == D_GREET and mock.talk.log[1].who == "npc" and mock.talk.log[1].id == "0", "NPC opens with the topic text")
opts = mock.talk_options()
check(#opts == 1 and opts[1] == "a1", "actor answers a1")
mock.talk_say("a1")
check(mock.talk.log[#mock.talk.log].id == "w1" and mock.talk.current == nil, "NPC w1 (leaf) ends the dialog")
mock.talk_close()
mock.ticks(2)
check(core.quest_status(UN) == "completed", "quest completed")
check(wolf:get_start_dialog() == nil, "start dialog restored when the topic finished")
if (failed > 0) then fail_dump() end

-- ============================================================================ (o) nq reload
section("(o) nq reload invalidates and re-registers dialogs; refused mid-talk, retried on close")
setup()
mock.first_update()
core = xms_nq
mock.dialog_load(D_MEET)
check(mock.dialog_shared[D_MEET] ~= nil, "graph cached")
xms_nq_console.exec("reload")
check(mock.dialog_shared[D_MEET] == nil and (mock.dialog_invalidated[D_MEET] or 0) >= 2, "reload invalidated the cached graph")
check(mock.dialogs_registered[D_MEET] ~= nil and mock.dialogs_registered[D_REPORT] ~= nil, "dialogs still registered after reload")
mock.talk_open(wolf)
mock.talk_topics()
check(mock.dialog_shared[D_MEET] ~= nil, "graph rebuilt on the next talk")
xms_nq_console.exec("reload")
check(mock.dialog_shared[D_MEET] ~= nil and mock.log_has("could not be invalidated now"), "invalidate refused while talking, warning logged")
mock.talk_close()
check(mock.dialog_shared[D_MEET] == nil, "retried when the talk closed")
if (failed > 0) then fail_dump() end

-- ============================================================================ (p) tasks lost by the engine
section("(p) task recreated when the actor registry lost it")
setup()
mock.first_update()
core = xms_nq
check(mock.task_by_id(T_BREAD) ~= nil, "task given")
mock.tasks_lost()
check(mock.task_by_id(T_BREAD) == nil, "registry emptied")
mock.news = {}
mock.rebuild()
mock.first_update()
core = xms_nq
tb = mock.task_by_id(T_BREAD)
check(tb ~= nil and tb:get_state() == task.in_progress and tb:get_title() == mock.cp("Хлеб для новичков"), "task recreated from the declaration on init")
check(mock.log_has("task bring_bread was missing from the PDA, recreated"), "recreation logged")
check(task_news_count("new", T_BREAD) == 1, "recreated task emits one new-task notification")
check(core.quest_state(UID_A).tasks.bring_bread == "active", "status active")
-- task.give on an already active task only refreshes it, no second registry entry
local before_n = #db.actor._tasks
local before_new = task_news_count("new", T_BREAD)
core.run_actions(core.make_ctx(UID_A, "intro"), { { kind = "task.give", params = { task = "bring_bread" } } }, "enter")
check(#db.actor._tasks == before_n and task_news_count("new", T_BREAD) == before_new, "second task.give is idempotent")
-- task.remove: silent, status none
core.run_actions(core.make_ctx(UID_A, "intro"), { { kind = "task.remove", params = { task = "bring_bread" } } }, "enter")
check(core.quest_state(UID_A).tasks.bring_bread == nil and mock.task_by_id(T_BREAD):get_state() == task.fail and task_news_count("fail", T_BREAD) == 0, "task.remove closes silently, task_status none")
-- task.set_target / set_text on an active task
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.give", params = { task = "hunt" } } }, "enter")
local hunt_id = "nq." .. UID_B .. ".hunt"
local th = mock.task_by_id(hunt_id)
check(th ~= nil and th:get_map_object_id() == 65535, "hunt task without a target (ref boars not created yet)")
local before_update = task_news_count("updated", hunt_id)
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.set_target", params = { task = "hunt", target = { smart = "esc_smart_terrain_2_12" } } } }, "enter")
check(th:get_map_object_id() == mock.smarts["esc_smart_terrain_2_12"].id and th:get_map_location() == "secondary_task_location" and task_news_count("updated", hunt_id) == before_update + 1, "task.set_target -> map location and one updated news")
check(mock.spots[mock.smarts["esc_smart_terrain_2_12"].id .. "|ui_secondary_task_blink"] ~= nil, "blink spot added")
before_update = task_news_count("updated", hunt_id)
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.set_target", params = { task = "hunt", target = { smart = "esc_smart_terrain_2_12" } } } }, "enter")
check(task_news_count("updated", hunt_id) == before_update, "task.set_target suppresses a semantic no-op update")
-- hand-built action lists skip the loader, so the text is passed already converted (cp1251)
before_update = task_news_count("updated", hunt_id)
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.set_text", params = { task = "hunt", title = mock.cp("Новый заголовок"), descr = mock.cp("Новое описание") } } }, "enter")
check(th:get_title() == mock.cp("Новый заголовок") and th:get_description() == mock.cp("Новое описание") and task_news_count("updated", hunt_id) == before_update + 1, "task.set_text changes fields and emits one updated news")
before_update = task_news_count("updated", hunt_id)
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.set_text", params = { task = "hunt", title = mock.cp("Новый заголовок"), descr = mock.cp("Новое описание") } } }, "enter")
check(task_news_count("updated", hunt_id) == before_update, "task.set_text suppresses a semantic no-op update")
local hunt_def = core.quest_def(UID_B).tasks.hunt
local saved_target = hunt_def.target
hunt_def.target = { smart = "esc_smart_terrain_2_12" }
local smart_id = mock.smarts["esc_smart_terrain_2_12"].id
level.map_remove_object_spot(smart_id, "ui_secondary_task_blink")
local map_add = level.map_add_object_spot
local blink_adds = 0
level.map_add_object_spot = function(...)
	blink_adds = blink_adds + 1
	return map_add(...)
end
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.give", params = { task = "hunt" } } }, "enter")
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.give", params = { task = "hunt" } } }, "enter")
level.map_add_object_spot = map_add
hunt_def.target = saved_target
check(blink_adds == 1, "repeated task.give keeps one blink spot")
before_update = task_news_count("updated", hunt_id)
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.set_target", params = { task = "hunt" } } }, "enter")
check(th:get_map_object_id() == 65535 and task_news_count("updated", hunt_id) == before_update + 1, "task.set_target clear emits one updated news")
before_update = task_news_count("updated", hunt_id)
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.set_target", params = { task = "hunt" } } }, "enter")
check(th:get_map_object_id() == 65535 and task_news_count("updated", hunt_id) == before_update, "task.set_target suppresses a repeated clear update")
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.fail", params = { task = "hunt" } } }, "enter")
check(th:get_state() == task.fail and core.quest_state(UID_B).tasks.hunt == "failed" and task_news_count("fail", hunt_id) == 1, "task.fail emits one failure notification")
-- undeclared task -> error logged, nothing else breaks
core.run_actions(core.make_ctx(UID_B, "hunt"), { { kind = "task.give", params = { task = "nope" } } }, "enter")
check(mock.log_has("task 'nope' is not declared"), "undeclared task errors at runtime")
if (failed > 0) then fail_dump() end

-- ============================================================================ (p2) reset/orphan task cleanup
section("(p2) reset and orphan cleanup close tasks without failure notifications")
setup()
mock.first_update()
core = xms_nq
tb = mock.task_by_id(T_BREAD)
core.reset(UID_A)
check(tb:get_state() == task.fail and task_news_count("fail", T_BREAD) == 0, "quest reset closes its task silently")

setup()
mock.first_update()
mock.ticks(1)
mock.news = {}
mock.deleted["mod_a/linear_fetch.nqasset"] = true
mock.rebuild()
mock.first_update()
tb = mock.task_by_id(T_BREAD)
check(tb:get_state() == task.fail and task_news_count("fail", T_BREAD) == 0, "orphan cleanup closes its task silently")
check(mock.log_has("quest is no longer installed, its PDA task bring_bread was closed"), "orphan cleanup logged")
if (failed > 0) then fail_dump() end

-- ============================================================================ (q) errors never propagate
section("(q) precondition/action errors are contained; the assert guard holds even when the quest ends mid-talk")
setup()
mock.overrides["mod_a/errd.nqasset"] = [[return { nq = 1, id = "errd", vars = { x = 0 }, nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "t" } },
	{ id = "t", kind = "dialog.topic", params = { npc = { story = "esc_2_12_stalker_wolf" }, text = "Проверка." }, out = { next = "n1" } },
	{ id = "n1", kind = "dialog.npc_phrase", params = { text = "n1" }, on_exit = { { kind = "lua", params = { code = "error('act boom')" } } }, out = { next = { "a_bad", "a_ok" } } },
	{ id = "a_bad", kind = "dialog.actor_phrase", params = { text = "bad" }, cond = { { kind = "lua", params = { code = "error('pre boom')" } } }, out = { next = "n2" } },
	{ id = "a_ok", kind = "dialog.actor_phrase", params = { text = "ok" }, out = { next = "n2" } },
	{ id = "n2", kind = "dialog.npc_phrase", params = { text = "n2" }, out = { next = "a_end" } },
	{ id = "a_end", kind = "dialog.actor_phrase", params = { text = "end" } },
} }]]
mock.first_update()
core = xms_nq
local UERR, D_ERR = "mod_a.errd", "nq.mod_a.errd.t"
mock.talk_open(wolf)
mock.talk_start(D_ERR)
check(mock.log_has("errd/n1: exit#0 lua: .*act boom"), "phrase on_exit error logged, talk goes on")
check(mock.log_has("errd/a_bad: cond lua: .*pre boom"), "precondition error logged")
opts = mock.talk_options()
check(#opts >= 1 and opts[1] == "a_ok", "erroring precondition reads false, unconditional phrase stays")
check(#opts == 2 and string.find(opts[2], "^nq_auto_"), "the group with a filtered phrase keeps its fallback leaf")
-- the quest ends while the window is open: unconditional phrases still pass, actions are ignored
core.finish_quest(UERR, "failed", false)
mock.talk_say("a_ok")
check(mock.talk.log[#mock.talk.log].id == "n2", "n2 (unconditional) offered and said although the quest is failed")
opts = mock.talk_options()
check(#opts == 1 and opts[1] == "a_end", "a_end still available")
mock.talk_say("a_end")
check(mock.talk.current == nil and core.quest_status(UERR) == "failed", "leaf said, quest untouched")
mock.talk_close()
-- entry points tolerate garbage
check(xms_nq_dialog.pre(nil, nil, "no.such.dialog", "0", "x") == true, "pre for an unknown dialog does not filter")
check(xms_nq_dialog.pre(nil, nil, D_ERR, "n1", "nq_auto_n1") == true, "pre for an automatic reply is true")
check(xms_nq_dialog.pre(nil, nil, D_ERR, "n1", "a_bad") == false, "pre for a conditional phrase of a finished quest is false")
xms_nq_dialog.act(nil, nil, "no.such.dialog", "0")
xms_nq_dialog.act(nil, nil, D_ERR, "a_ok")
check(xms_nq_dialog.text(nil, nil, "no.such.dialog", "p") == "p" and xms_nq_dialog.text(nil, nil, D_ERR, "n1") == "n1", "text falls back to the phrase id / static text without speakers")
check(type(xms.dialogs_for(nil, nil)) == "table", "dialogs_for(nil) returns a table")
-- world kinds through run_actions
section("(q2) world extras: spawn.squad/squad.move/squad.remove, spawn.object, npc.kill/remove, relations")
local ctxw = core.make_ctx(UID_B, "hunt")
qs = core.quest_state(UID_B)
core.run_actions(ctxw, { { kind = "spawn.squad", params = { section = "simulation_boar", smart = "esc_smart_terrain_2_12", ref = "guards", hold = true } } }, "enter")
local guards = qs.refs.guards and mock.se[qs.refs.guards.id]
check(guards ~= nil and guards.scripted_target == "esc_smart_terrain_2_12", "spawn.squad with hold")
core.run_actions(ctxw, { { kind = "squad.move", params = { target = { ref = "guards" }, smart = "esc_smart_terrain_3_16" } } }, "enter")
check(guards.scripted_target == "esc_smart_terrain_3_16" and qs.refs.guards.scripted == "esc_smart_terrain_3_16", "squad.move -> scripted_target and ref record")
core.run_actions(ctxw, { { kind = "squad.move", params = { target = { ref = "guards" }, follow_actor = true } } }, "enter")
check(guards.scripted_target == "actor", "squad.move follow_actor")
core.run_actions(ctxw, { { kind = "relation.set", params = { who = { ref = "guards" }, value = "enemy" } } }, "enter")
check(mock.squad_relations[guards.id] == "enemy", "relation.set on a squad ref")
core.run_actions(ctxw, { { kind = "squad.remove", params = { target = { ref = "guards" } } } }, "enter")
check(mock.se[guards.id] == nil, "squad.remove released the squad")
core.run_actions(ctxw, { { kind = "spawn.object", params = { section = "medkit", place = { level = "l01_escape", pos = { 1, 2, 3 } }, ref = "box" } } }, "enter")
check(qs.refs.box and mock.se[qs.refs.box.id] and mock.se[qs.refs.box.id]._section == "medkit", "spawn.object created + ref")
local victim = mock.add_npc("victim_npc", "victim_sid", { community = "bandit" })
core.run_actions(ctxw, { { kind = "relation.set", params = { who = { story = "victim_sid" }, value = "friend" } } }, "enter")
check(victim._goodwill == 1000, "relation.set friend -> force_set_goodwill 1000")
core.run_actions(ctxw, { { kind = "relation.goodwill", params = { who = { story = "victim_sid" }, delta = -300 } } }, "enter")
check(victim._goodwill == 700, "relation.goodwill delta on the NPC")
core.run_actions(ctxw, { { kind = "relation.set", params = { who = { community = "bandit" }, value = "enemy" } } }, "enter")
check(mock.faction_relations["bandit>stalker"] == -5000, "relation.set community -> set_factions_community")
core.run_actions(ctxw, { { kind = "relation.goodwill", params = { who = { community = "bandit" }, delta = 50 } } }, "enter")
check(mock.faction_goodwill.bandit == 50, "relation.goodwill community -> change_factions_community_num")
core.run_actions(ctxw, { { kind = "npc.kill", params = { npc = { story = "victim_sid" } } } }, "enter")
check(victim:alive() == false, "npc.kill online")
core.run_actions(ctxw, { { kind = "npc.remove", params = { npc = { story = "victim_sid" } } } }, "enter")
check(mock.se[victim:id()] == nil, "npc.remove released the server object")
-- objective.kill on a story NPC killed by the actor, by_actor honoured
mock.overrides["mod_a/kills.nqasset"] = [[return { nq = 1, id = "kills", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "k" } },
	{ id = "k", kind = "objective.kill", params = { target = { story = "boss_sid" }, by_actor = true }, out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
local boss = mock.add_npc("boss_npc", "boss_sid", { community = "bandit" })
xms_nq_console.exec("reload")
core = xms_nq
local UK = "mod_a.kills"
qs = core.quest_state(UK)
check(qs and qs.tokens.k ~= nil and qs.tokens.k.w.id == boss:id() and qs.tokens.k.w.kind == "npc", "kill waits on the story NPC")
boss:kill(other_npc or wolf)
mock.ticks(2)
qs = core.quest_state(UK)
check(qs.tokens.k ~= nil, "killed by somebody else: by_actor keeps waiting")
local boss2 = mock.add_npc("boss_npc", "boss2_sid", { community = "bandit" })
qs.tokens.k.w.id = boss2:id()	-- retarget the token for the second half of the check
boss2:kill(db.actor)
mock.ticks(2)
check(core.quest_status(UK) == "completed", "killed by the actor -> done -> completed")
-- dialog.force / dialog.break
core.run_actions(ctxw, { { kind = "dialog.force", params = { npc = { story = "esc_2_12_stalker_wolf" }, allow_break = false } } }, "enter")
check(#mock.forced_talks == 1 and mock.forced_talks[1].id == wolf:id() and mock.forced_talks[1].disable_break == true and mock.talking, "dialog.force -> run_talk_dialog(npc, disable_break)")
core.run_actions(core.make_ctx(UID_B, "hunt", wolf), { { kind = "dialog.break", params = {} } }, "enter")
check(not mock.talking, "dialog.break closed the talk")
if (failed > 0) then fail_dump() end

-- ============================================================================ (r) topic ordering, repeatable topics, talk events
section("(r) topics of several quests in uid order, once=false topic with cond, root-only topic, talk_started/talk_ended")
setup()
mock.overrides["mod_a/zz_second.nqasset"] = [[return { nq = 1, id = "zz_second", vars = { talks = 0, ends = 0, passes = 0, ready = false }, nodes = {
	{ id = "start", kind = "trigger.start", out = { next = { "rep", "solo" } } },
	{ id = "rep", kind = "dialog.topic", once = false, params = { npc = { story = "esc_2_12_stalker_wolf" }, text = "Повторяемая тема." },
	  cond = { { kind = "var", params = { name = "ready", op = "eq", value = true } } },
	  on_exit = { { kind = "var.add", params = { name = "passes", delta = 1 } } },
	  out = { next = "r1", done = "after" } },
	{ id = "r1", kind = "dialog.npc_phrase", params = { text = "Снова ты." } },
	{ id = "after", kind = "flow.step", once = false, on_enter = { { kind = "news.tip", params = { text = "после темы" } } } },
	{ id = "solo", kind = "dialog.topic", params = { npc = { story = "esc_2_12_stalker_wolf" }, text = "Просто фраза без ответа." }, out = { done = "solo_done" } },
	{ id = "solo_done", kind = "flow.step", on_enter = { { kind = "news.tip", params = { text = "solo done" } } } },
	{ id = "on_talk", kind = "trigger.when", params = { ["repeat"] = true }, cond = { { kind = "event.talk_started", params = { npc = { story = "esc_2_12_stalker_wolf" } } } }, out = { next = "cnt" } },
	{ id = "cnt", kind = "flow.step", once = false, on_enter = { { kind = "var.add", params = { name = "talks", delta = 1 } } } },
	{ id = "on_end", kind = "trigger.when", params = { ["repeat"] = true }, cond = { { kind = "event.talk_ended", params = { npc = { story = "esc_2_12_stalker_wolf" } } } }, out = { next = "cnt2" } },
	{ id = "cnt2", kind = "flow.step", once = false, on_enter = { { kind = "var.add", params = { name = "ends", delta = 1 } } } },
} }]]
mock.first_update()
core = xms_nq
local UZ = "mod_a.zz_second"
local D_REP, D_SOLO = "nq." .. UZ .. ".rep", "nq." .. UZ .. ".solo"
qs = core.quest_state(UZ)
check(qs and qs.tokens.rep ~= nil and qs.tokens.solo ~= nil, "both topics wait")
mock.talk_open(wolf)
ids = mock.talk_topics()
check(#ids == 2 and ids[1] == D_MEET and ids[2] == D_SOLO, "uid order: dialog_branching.meet before zz_second.solo; rep hidden by its cond (" .. table.concat(ids, ",") .. ")")
mock.talk_topics()
mock.ticks(1)
check(core.quest_state(UZ).vars.talks == 1, "talk_started once per talk window even though the list was asked twice")
core.set_var(UZ, "ready", true)
ids = mock.talk_topics()
check(#ids == 3 and ids[2] == D_REP and ids[3] == D_SOLO, "cond true: rep appears in node order (" .. table.concat(ids, ",") .. ")")
-- repeatable topic: leaf said -> on_exit + done pin, token stays, topic offered again
mock.talk_start(D_REP)
check(mock.talk.log[#mock.talk.log].id == "r1" and mock.talk.current == nil, "r1 leaf said")
qs = core.quest_state(UZ)
check(qs.vars.passes == 1, "topic on_exit ran (synchronously, repeatable path)")
mock.tick(50)
qs = core.quest_state(UZ)
check(qs.tokens.rep ~= nil and qs.done.rep == nil, "token stays on the repeatable topic")
check(mock.news_has(mock.cp("после темы")), "done pin target entered")
check(has(mock.talk_topics(), D_REP), "repeatable topic offered again")
mock.talk_start(D_REP)
mock.tick(50)
check(core.quest_state(UZ).vars.passes == 2 and mock.news_count(mock.cp("после темы")) == 2, "second pass")
-- topic without phrases (W012): the root is the leaf
mock.talk_start(D_SOLO)
check(mock.talk.current == nil, "root said, dialog finished at once")
mock.tick(50)
qs = core.quest_state(UZ)
check(qs.tokens.solo == nil and qs.done.solo == "dialog.topic" and mock.news_has("solo done"), "root-only topic passed -> done pin")
mock.talk_close()
mock.ticks(1)
check(core.quest_state(UZ).vars.ends == 1, "talk_ended once")
mock.talk_open(wolf)
mock.talk_topics()
mock.talk_close()
mock.ticks(1)
qs = core.quest_state(UZ)
check(qs.vars.talks == 2 and qs.vars.ends == 2, "second talk counted")
if (failed > 0) then fail_dump() end

-- ============================================================================ (s) trigger.when "repeat"
-- The parameter is a Lua keyword, so it only reads back as params["repeat"]; the edge state
-- (trig.last) must latch for a one-shot trigger and re-arm for a repeating one, on both the
-- polled path and the event path.
section("(s) trigger.when repeat: rising edge, latch when repeat=false, re-arm when true")
setup()
local function trig_quest(id, params, cond)
	return "return { nq = 1, id = \"" .. id .. "\", vars = { n = 0 }, nodes = {\n" ..
		"{ id = \"t\", kind = \"trigger.when\", params = " .. params .. ", cond = " .. cond .. ", out = { next = \"s\" } },\n" ..
		"{ id = \"s\", kind = \"flow.step\", on_enter = { { kind = \"var.add\", params = { name = \"n\", delta = 1 } } } },\n} }"
end
local C_ITEM = [[{ { kind = "has_item", params = { section = "bread" } } }]]
local C_SIG = [[{ { kind = "event.signal", params = { name = "go" } } }]]
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/lvl_once.nqasset"] = trig_quest("lvl_once", "{}", C_ITEM)
mock.overrides["mod_a/lvl_rep.nqasset"] = trig_quest("lvl_rep", [[{ ["repeat"] = true }]], C_ITEM)
mock.overrides["mod_a/ev_once.nqasset"] = trig_quest("ev_once", "{}", C_SIG)
mock.overrides["mod_a/ev_rep.nqasset"] = trig_quest("ev_rep", [[{ ["repeat"] = true }]], C_SIG)
mock.overrides["mod_a/ev_cd.nqasset"] = trig_quest("ev_cd", [[{ ["repeat"] = true, cooldown = { seconds = 1 } }]], C_SIG)
mock.first_update()
core = xms_nq
local function hits(id) local q = core.quest_state("mod_a." .. id) return q and q.vars.n end
local function edge(id) local q = core.quest_state("mod_a." .. id) return q and q.trig.t and q.trig.t.last end
local function signal()
	core.emit({ name = "signal", signal = "go", module = "mod_a" })
	mock.ticks(2)
end

check(core.quest_def("mod_a.lvl_rep").node_by_id.t.params["repeat"] == true, "repeat survives the loader as params['repeat']")
check(core.quest_def("mod_a.lvl_once").node_by_id.t.params["repeat"] == false, "repeat defaults to false from the catalog")
check(hits("lvl_once") == 0 and edge("lvl_once") == false, "armed: edge starts false, nothing fired")

-- polled path: rising edge
local loaf = mock.add_item("bread")
mock.ticks(2)
check(hits("lvl_once") == 1 and hits("lvl_rep") == 1, "polled: both fire on the rising edge")
check(edge("lvl_once") == true and edge("lvl_rep") == true, "polled: edge latched after firing")
mock.ticks(3)
check(hits("lvl_rep") == 1, "polled: level stays true -> no refire")
-- falling edge: only the repeating trigger re-arms
mock.remove_item(loaf)
mock.ticks(2)
check(edge("lvl_rep") == false, "polled: repeat trigger re-armed on the falling edge")
check(edge("lvl_once") == true, "polled: one-shot trigger stays latched on the falling edge")
mock.add_item("bread")
mock.ticks(2)
check(hits("lvl_rep") == 2, "polled: repeat trigger fires on the second rising edge")
check(hits("lvl_once") == 1, "polled: one-shot trigger does not fire twice")

-- event path: an event cond is true only for its instant, so a repeat trigger re-arms at once
signal()
check(hits("ev_once") == 1 and hits("ev_rep") == 1 and hits("ev_cd") == 1, "event: all three fire on the first signal")
check(edge("ev_once") == true, "event: one-shot trigger latched")
check(edge("ev_rep") == false, "event: repeat trigger re-armed for the next event")
signal()
check(hits("ev_once") == 1, "event: one-shot trigger does not fire on the second signal")
check(hits("ev_rep") == 2, "event: repeat trigger fires on the second signal")
check(hits("ev_cd") == 1, "event: second signal inside the cooldown is swallowed")
mock.ticks(10)
signal()
check(hits("ev_cd") == 2, "event: fires again once the cooldown expired")
check(core.quest_status("mod_a.ev_once") == "completed", "one-shot trigger quest completes when nothing is armed")
check(core.quest_status("mod_a.ev_rep") == "active", "repeat trigger keeps its quest active")
if (failed > 0) then fail_dump() end

-- ============================================================================ (t) cp1251 conversion
-- The engine's script loader does not turn a decimal escape into a byte: "\208" arrives in the Lua
-- state as the three characters 2, 0, 8. Anything in the runtime that has to talk about raw bytes
-- must therefore build them with string.char, which is what this section pins down. Every value
-- below is assembled that way on purpose - written as escapes the test would pass in the harness
-- (LuaJIT loads files directly here) and still ship a runtime that never converts anything.
section("(t) UTF-8 -> cp1251 conversion works on byte values built at run time")

local function B(...)
	local t = { ... }
	for i = 1, #t do t[i] = string.char(t[i]) end
	return table.concat(t)
end

-- setup() rebuilds the script namespaces, so the reference quests are reloaded first and every
-- name below comes from the fresh state.
setup()
mock.first_update()
local u = xms_nq_util
local conv, lost = u.to_cp1251(B(208, 161, 208, 187, 209, 139, 209, 136, 208, 176, 208, 187))	-- "Слышал"
check(conv == B(209, 235, 251, 248, 224, 235) and lost == 0, "cyrillic converts to cp1251")
conv, lost = u.to_cp1251(B(208, 129, 209, 145))													-- "Ёё"
check(conv == B(168, 184) and lost == 0, "Ё/ё take their cp1251 slots")
conv, lost = u.to_cp1251(B(226, 128, 148, 226, 132, 150, 226, 128, 166))						-- "—№…"
check(conv == B(151, 185, 133) and lost == 0, "punctuation outside the cyrillic block converts")
conv, lost = u.to_cp1251("plain ascii")
check(conv == "plain ascii" and lost == 0, "ascii passes through untouched")
conv, lost = u.to_cp1251(B(240, 159, 146, 128))													-- an emoji
check(conv == "?" and lost == 1, "a character with no cp1251 equivalent becomes '?' and is counted")

-- The whole load path: a reference quest's title and a topic text must reach the runtime as cp1251,
-- with no UTF-8 lead byte left anywhere. Earlier sections replaced mod_a with inline assets, so the
-- reference quests are reloaded from docs\nq\examples first.
setup()
mock.first_update()
local qd = xms_nq.quest_def("mod_a.dialog_branching")
local topic = qd and qd.node_by_id and qd.node_by_id.meet
local lead = "[" .. B(208, 209) .. "][" .. string.char(128) .. "-" .. string.char(191) .. "]"
check(qd ~= nil and string.sub(qd.title, 1, 4) == B(196, 238, 235, 227), "quest title loaded as cp1251")
check(qd ~= nil and not string.find(qd.title, lead), "quest title carries no UTF-8 lead pair")
check(topic ~= nil and not string.find(topic.params.text, lead), "topic text carries no UTF-8 lead pair")
check(topic ~= nil and string.sub(topic.params.text, 1, 6) == B(209, 235, 251, 248, 224, 235),
	"topic text is the cp1251 form of the asset's UTF-8")
if (failed > 0) then fail_dump() end

-- ============================================================================ (u) item.give ammo
-- `count` is whole items on both transfer paths, so for ammo it is whole boxes. Outside a dialog the
-- give goes through util.give_items -> create_ammo, which counts ROUNDS; inside one through
-- dialogs.relocate_item_section_to_actor, which creates whole objects. The totals must not disagree.
section("(u) item.give: count is whole boxes for ammo, on the world path and in a dialog")
setup()
mock.first_update()
core = xms_nq
local AMMO, BOX, NBOX = "ammo_9x18_fmj", 30, 2
local ctxg = core.make_ctx(UID_B, "hunt")				-- no npc and nobody talking -> world path
core.run_actions(ctxg, { { kind = "item.give", params = { section = AMMO, count = NBOX } } }, "enter")
local w_rounds, w_objs = mock.ammo_rounds(AMMO), mock.count_items(AMMO)
check(w_rounds == NBOX * BOX, "world item.give: count=2 -> 2 boxes = 60 rounds (" .. w_rounds .. ")")
check(w_objs == NBOX, "world item.give: 2 ammo objects (" .. w_objs .. ")")
core.run_actions(ctxg, { { kind = "item.give", params = { section = "wpn_pm", count = 3 } } }, "enter")
check(mock.count_items("wpn_pm") == 3, "world item.give: a non-ammo section gives count objects (" .. mock.count_items("wpn_pm") .. ")")
-- the same section and the same count inside a dialog must land on the same totals
mock.talk_open(wolf)
local ctxt = core.make_ctx(UID_B, "hunt", wolf)
core.run_actions(ctxt, { { kind = "item.give", params = { section = AMMO, count = NBOX } } }, "enter")
local d_rounds, d_objs = mock.ammo_rounds(AMMO) - w_rounds, mock.count_items(AMMO) - w_objs
check(d_rounds == NBOX * BOX, "dialog item.give: count=2 -> 2 boxes = 60 rounds (" .. d_rounds .. ")")
check(d_rounds == w_rounds and d_objs == w_objs, "both paths agree on count=2 (world " .. w_rounds .. "r/" .. w_objs .. "o, dialog " .. d_rounds .. "r/" .. d_objs .. "o)")
core.run_actions(ctxt, { { kind = "item.give", params = { section = "wpn_pm", count = 3 } } }, "enter")
check(mock.count_items("wpn_pm") == 6, "dialog item.give: a non-ammo section gives count objects (" .. mock.count_items("wpn_pm") .. ")")
mock.talk_close()
-- a box_size that cannot be read must degrade to whole objects, never to nothing
local BAD = "ammo_broken_box"
core.run_actions(ctxg, { { kind = "item.give", params = { section = BAD, count = NBOX } } }, "enter")
check(mock.count_items(BAD) == NBOX, "unreadable box_size falls back to count objects (" .. mock.count_items(BAD) .. ")")
check(mock.relocated("in", BAD) ~= nil, "unreadable box_size still reported a transfer (give_items returned > 0)")
if (failed > 0) then fail_dump() end

-- ============================================================================ (v) trigger.when "off"
-- The falling edge is a second pin, not a second firing: it routes without counting in `fired`,
-- it is owed at most once per rising edge, and only a wired `off` keeps a spent trigger polled
-- (and its quest active). The owed edge is plain state in qs.trig, so it rides through a save -
-- and a save written before all this simply owes nothing.
section("(v) trigger.when off: falling edge, once per rising edge, only when wired, saved")
setup()
local function off_quest(id, params, cond, out, with_gone)
	local nodes = "{ id = \"t\", kind = \"trigger.when\", params = " .. params .. ", cond = " .. cond .. ", out = " .. out .. " },\n" ..
		"{ id = \"on\", kind = \"flow.step\", on_enter = { { kind = \"var.add\", params = { name = \"n\", delta = 1 } } } },\n"
	if (with_gone) then
		nodes = nodes .. "{ id = \"gone\", kind = \"flow.step\", on_enter = { { kind = \"var.add\", params = { name = \"g\", delta = 1 } } } },\n"
	end
	return "return { nq = 1, id = \"" .. id .. "\", vars = { n = 0, g = 0 }, nodes = {\n" .. nodes .. "} }"
end
local C_BREAD = [[{ { kind = "has_item", params = { section = "bread" } } }]]
local C_PM = [[{ { kind = "has_item", params = { section = "wpn_pm" } } }]]
local C_BAND = [[{ { kind = "has_item", params = { section = "bandage" } } }]]
local C_NEVER = [[{ { kind = "has_item", params = { section = "vodka" } } }]]
local OUT_BOTH, OUT_NEXT = [[{ next = "on", off = "gone" }]], [[{ next = "on" }]]
local REP = [[{ ["repeat"] = true }]]
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/off_once.nqasset"] = off_quest("off_once", "{}", C_BREAD, OUT_BOTH, true)
mock.overrides["mod_a/off_rep.nqasset"] = off_quest("off_rep", REP, C_BREAD, OUT_BOTH, true)
mock.overrides["mod_a/off_plain.nqasset"] = off_quest("off_plain", "{}", C_BREAD, OUT_NEXT, false)
mock.overrides["mod_a/off_never.nqasset"] = off_quest("off_never", "{}", C_NEVER, OUT_BOTH, true)
mock.overrides["mod_a/off_pre.nqasset"] = off_quest("off_pre", "{}", C_PM, OUT_BOTH, true)
mock.overrides["mod_a/off_old.nqasset"] = off_quest("off_old", "{}", C_BAND, OUT_BOTH, true)
local pm = mock.add_item("wpn_pm")		-- already true when the triggers are armed
mock.first_update()
core = xms_nq
local function V(id, v) local q = core.quest_state("mod_a." .. id) return q and q.vars[v] end
local function owe(id) local q = core.quest_state("mod_a." .. id) return q and q.trig.t and q.trig.t.owe end
local function st(id) return core.quest_status("mod_a." .. id) end

check(has(core.quest_def("mod_a.off_once").node_by_id.t.spec.pins, "off"), "the catalog declares the off pin")
local off_errs = 0
for _, p in ipairs(core.quest_def("mod_a.off_once").problems) do
	if (p.severity == "E") then off_errs = off_errs + 1 end
end
check(off_errs == 0, "wiring off is not an error for the loader")
mock.ticks(2)
check(V("off_pre", "n") == 1 and V("off_pre", "g") == 0, "a condition already true at arm fires next and no spurious off")
check(owe("off_pre") == "off", "the rising edge owes the falling one")
check(V("off_never", "n") == 0 and V("off_never", "g") == 0 and owe("off_never") == nil, "a condition that was never true owes nothing")

-- rising edge
local loaf = mock.add_item("bread")
mock.ticks(2)
check(V("off_once", "n") == 1 and V("off_rep", "n") == 1 and V("off_plain", "n") == 1, "rising edge: next on all three")
check(V("off_once", "g") == 0 and V("off_rep", "g") == 0, "no off while the condition holds")
check(st("off_plain") == "completed", "an unwired off changes nothing: the spent one-shot quest completes as before")
check(st("off_once") == "active" and owe("off_once") == "off", "a wired off keeps the spent trigger and its quest alive")
mock.ticks(4)
check(V("off_once", "g") == 0 and V("off_rep", "g") == 0, "still no off several polls later")

-- falling edge
mock.remove_item(loaf)
mock.ticks(2)
check(V("off_once", "g") == 1 and V("off_rep", "g") == 1, "falling edge: off fires, one-shot trigger included")
check(V("off_once", "n") == 1 and V("off_rep", "n") == 1, "the falling edge is not a second firing")
local qso = core.quest_state("mod_a.off_once")
check(qso.fired.t == 1 and qso.done.t == nil, "off did not count in fired and did not mark the node done")
check(owe("off_once") == nil, "nothing is owed once the off went out")
mock.ticks(4)
check(V("off_once", "g") == 1 and V("off_rep", "g") == 1, "off fires at most once per rising edge")
check(st("off_once") == "completed", "the quest ends on the check right after its last edge")

-- the repeat trigger goes round again, the finished one stays put
local loaf2 = mock.add_item("bread")
mock.ticks(2)
check(V("off_rep", "n") == 2 and V("off_rep", "g") == 1, "repeat trigger: second rising edge")
check(V("off_once", "n") == 1 and V("off_once", "g") == 1, "the finished quest does not move")
mock.remove_item(loaf2)
mock.ticks(2)
check(V("off_rep", "g") == 2, "repeat trigger: second falling edge")

-- save / load with the edge still owed (off_pre is holding its pistol)
mock.rebuild()
mock.first_update()
core = xms_nq
mock.ticks(1)
check(st("off_pre") == "active" and owe("off_pre") == "off", "the owed falling edge survives a load")
mock.remove_item(pm)
mock.ticks(2)
check(V("off_pre", "g") == 1 and V("off_pre", "n") == 1, "the reloaded trigger is polled again and delivers its off")
check(st("off_pre") == "completed", "and the quest ends there")

-- a save written before the off pin: the trigger is spent and owes nothing
local band = mock.add_item("bandage")
mock.ticks(2)
check(V("off_old", "n") == 1 and owe("off_old") == "off", "off_old fired and owes its edge")
core.quest_state("mod_a.off_old").trig.t.owe = nil		-- the field an older save simply does not have
core.save_now()
mock.rebuild()
mock.first_update()
core = xms_nq
mock.ticks(1)
check(st("off_old") == "completed", "an older save: the spent trigger is torn down exactly as it used to be")
mock.remove_item(band)
mock.ticks(2)
check(V("off_old", "g") == 0, "and no off arrives out of nowhere")
if (failed > 0) then fail_dump() end

-- ============================================================================ (w) objective.fetch forms
-- The plain section+count form is untouched (section (a) drives it end to end); these are the two
-- forms that follow concrete objects: one named item, and "only what came out of THIS container".
section("(w) objective.fetch: a named item, a named container, and an identical item from elsewhere")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/f_item.nqasset"] = [[return { nq = 1, id = "f_item", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "make" } },
	{ id = "make", kind = "flow.step", on_enter = { { kind = "item.spawn", params = { section = "bread", place = { level = "l01_escape", pos = { 10, 0, 10 } }, ref = "prize" } } }, out = { next = "get" } },
	{ id = "get", kind = "objective.fetch", params = { item = { ref = "prize" } }, out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.overrides["mod_a/f_story.nqasset"] = [[return { nq = 1, id = "f_story", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "get" } },
	{ id = "get", kind = "objective.fetch", params = { item = { story = "quest_doc" } }, out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.overrides["mod_a/f_from.nqasset"] = [[return { nq = 1, id = "f_from", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "loot" } },
	{ id = "loot", kind = "objective.fetch", params = { section = "bread", count = 2, from = { story = "wolf_stash" } }, out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.overrides["mod_a/f_plain.nqasset"] = [[return { nq = 1, id = "f_plain", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "get" } },
	{ id = "get", kind = "objective.fetch", params = { section = "medkit", count = 2 }, out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
local stash = mock.add_container("inventory_box", "wolf_stash")
for _ = 1, 3 do mock.put_in_container(stash, "bread") end
local decoy = mock.add_container("inventory_box", "decoy_stash")	-- the same bread, the wrong box
mock.put_in_container(decoy, "bread")
local doc = mock.new_se("wpn_pm", nil, 1, 1, nil)
mock.sor:register(doc.id, "quest_doc")
mock.first_update()
core = xms_nq
local UFI, UFS, UFF, UFP = "mod_a.f_item", "mod_a.f_story", "mod_a.f_from", "mod_a.f_plain"
local keys = xms_nq_util.count_keys
local function tok_of(uid, node) local q = core.quest_state(uid) return q and q.tokens[node] or nil end
qs = core.quest_state(UFI)
local prize_id = qs.refs.prize and qs.refs.prize.id
check(qs.tokens.get ~= nil and prize_id ~= nil and mock.se[prize_id] ~= nil, "fetch by item waits on the object item.spawn created")
check(tok_of(UFF, "loot") ~= nil and tok_of(UFF, "loot").w.cid == stash.id, "fetch from container resolved the stash by story id")
check(keys(tok_of(UFF, "loot").w.seen) == 3, "the three loaves inside the stash are watched (" .. keys(tok_of(UFF, "loot").w.seen) .. ")")
-- the negative case: an identical item that never was in the stash and is not the named object
mock.add_item("bread", true)
mock.ticks(2)
check(tok_of(UFI, "get") ~= nil, "another bread does not satisfy fetch by item")
check(tok_of(UFF, "loot") ~= nil and (tok_of(UFF, "loot").w.n or 0) == 0, "and it does not count towards the container either")
mock.pick_up(mock.se[prize_id], true)
mock.ticks(2)
check(core.quest_status(UFI) == "completed", "picking up that exact object completes fetch by item")
check(tok_of(UFS, "get") ~= nil, "fetch by story item still waits")
mock.pick_up(doc, true)
mock.ticks(2)
check(core.quest_status(UFS) == "completed", "the story object in the inventory completes it")
-- looting the container itself
mock.loot(stash, "bread", true)
mock.ticks(2)
check(tok_of(UFF, "loot") ~= nil and tok_of(UFF, "loot").w.n == 1, "one loaf out of the stash counted (" .. tostring(tok_of(UFF, "loot").w.n) .. ")")
check(keys(tok_of(UFF, "loot").w.seen) == 2, "two loaves left inside the stash")
check(mock.count_items("bread") == 3, "the actor carries three loaves, only one of them from the stash")
check(xms_nq_util.decode(mock.blobs["xms.nq"]).quests[UFF].tokens.loot.w.n == 1, "the container counter is in the staged blob")
mock.loot(decoy, "bread", true)
mock.ticks(2)
check(tok_of(UFF, "loot") ~= nil and tok_of(UFF, "loot").w.n == 1, "an identical loaf out of another container does not count")
mock.rebuild()
mock.first_update()
core = xms_nq
check(tok_of(UFF, "loot") ~= nil and tok_of(UFF, "loot").w.n == 1, "the container counter survived save/load")
mock.add_item("bread", true)
mock.ticks(2)
check(tok_of(UFF, "loot") ~= nil and tok_of(UFF, "loot").w.n == 1, "a bread from elsewhere after the load still does not count")
mock.loot(stash, "bread", true)
mock.ticks(2)
check(core.quest_status(UFF) == "completed", "the second loaf from the stash completes it")
-- and the original form is exactly what it was
check(tok_of(UFP, "get") ~= nil, "plain section+count fetch waiting")
mock.add_item("medkit", true)
mock.ticks(2)
check(tok_of(UFP, "get") ~= nil, "one medkit is not enough")
mock.add_item("medkit", true)
mock.ticks(2)
check(core.quest_status(UFP) == "completed", "plain section+count fetch behaves as before")
-- E022: the forms are mutually exclusive and one of them is required
local function fetch_codes(params)
	local q = xms_nq_load.load_asset("mod_x", "inline.nqasset",
		"return { nq = 1, id = \"x\", nodes = {\n" ..
		"{ id = \"s\", kind = \"trigger.start\", out = { next = \"g\" } },\n" ..
		"{ id = \"g\", kind = \"objective.fetch\", params = " .. params .. ", out = { done = \"e\" } },\n" ..
		"{ id = \"e\", kind = \"flow.end\" },\n} }")
	local set = {}
	for _, p in ipairs(q.problems) do set[p.code] = (set[p.code] or 0) + 1 end
	return set
end
check(fetch_codes([[{ section = "bread", count = 2 }]]).E022 == nil, "section+count is a valid form")
check(fetch_codes([[{ section = "bread", from = { story = "box" } }]]).E022 == nil, "section+from is a valid form")
check(fetch_codes([[{ item = { story = "doc" } }]]).E022 == nil, "item alone is a valid form")
check(fetch_codes([[{ item = { story = "doc" }, section = "bread" }]]).E022 == 1, "item + section -> E022")
check(fetch_codes([[{ item = { story = "doc" }, count = 2 }]]).E022 == 1, "item + count -> E022")
check(fetch_codes([[{ item = { story = "doc" }, from = { story = "box" } }]]).E022 == 1, "item + from -> E022")
check(fetch_codes([[{ from = { story = "box" } }]]).E022 == 1, "from without section -> E022")
check(fetch_codes([[{ item = "doc" }]]).E006 == 1, "item must be {ref=} or {story=} -> E006")
if (failed > 0) then fail_dump() end

-- ============================================================================ (x) objective.kill_count
section("(x) objective.kill_count: no filter, community, section, by_actor, counter across a save")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
local function kc_quest(id, params)
	return "return { nq = 1, id = \"" .. id .. "\", nodes = {\n" ..
		"{ id = \"start\", kind = \"trigger.start\", out = { next = \"k\" } },\n" ..
		"{ id = \"k\", kind = \"objective.kill_count\", params = " .. params .. ", out = { done = \"fin\" } },\n" ..
		"{ id = \"fin\", kind = \"flow.end\" },\n} }"
end
mock.overrides["mod_a/kc_any.nqasset"] = kc_quest("kc_any", "{ count = 2 }")
mock.overrides["mod_a/kc_comm.nqasset"] = kc_quest("kc_comm", [[{ count = 2, community = "bandit" }]])
mock.overrides["mod_a/kc_sect.nqasset"] = kc_quest("kc_sect", [[{ count = 1, section = "boss_npc" }]])
mock.overrides["mod_a/kc_other.nqasset"] = kc_quest("kc_other", "{ count = 2, by_actor = false }")
mock.first_update()
core = xms_nq
local function kc_n(id)
	local t = tok_of("mod_a." .. id, "k")
	return t and (t.w.n or 0) or nil
end
check(kc_n("kc_any") == 0 and kc_n("kc_comm") == 0, "counters start at zero")
check(tok_of("mod_a.kc_any", "k").b == true, "kill_count waits")
-- A bandit killed by the player counts everywhere it matches. No quest finishes in this tick and
-- no timer moves, so the blob is restaged only because the counter marked the state dirty itself.
local saves_before_kill = mock.save_calls
local b1 = mock.add_npc("bandit_npc", nil, { community = "bandit" })
b1:kill(db.actor)
mock.ticks(2)
check(kc_n("kc_any") == 1, "unfiltered: the player's kill counted (" .. tostring(kc_n("kc_any")) .. ")")
check(kc_n("kc_comm") == 1, "community filter: a bandit counted")
check(kc_n("kc_other") == 1, "by_actor = false counts the player's kill as well")
check(mock.save_calls > saves_before_kill and xms_nq_util.decode(mock.blobs["xms.nq"]).quests["mod_a.kc_any"].tokens.k.w.n == 1,
	"the counter marked the state dirty and went into the blob")
-- a kill by somebody else
local s1 = mock.add_npc("stalker_npc", nil, { community = "stalker" })
s1:kill(wolf)
mock.ticks(2)
check(kc_n("kc_any") == 1, "by_actor is on by default: an NPC-on-NPC kill does not count")
check(core.quest_status("mod_a.kc_other") == "completed", "by_actor = false counts a kill by an NPC and completes")
local b2 = mock.add_npc("bandit_npc", nil, { community = "bandit" })
b2:kill(wolf)
mock.ticks(2)
check(kc_n("kc_comm") == 1, "a matching community killed by an NPC does not count either")
-- the counter is part of the token and rides the blob
check(xms_nq_util.decode(mock.blobs["xms.nq"]).quests["mod_a.kc_any"].tokens.k.w.n == 1, "the kill counter is in the staged blob")
mock.rebuild()
mock.first_update()
core = xms_nq
check(kc_n("kc_any") == 1 and kc_n("kc_comm") == 1, "counters survived save/load")
local s2 = mock.add_npc("stalker_npc", nil, { community = "stalker" })
s2:kill(db.actor)
mock.ticks(2)
check(core.quest_status("mod_a.kc_any") == "completed", "second kill after the load completes the unfiltered quest")
check(kc_n("kc_comm") == 1, "the community filter ignored a stalker")
local b3 = mock.add_npc("bandit_npc", nil, { community = "bandit" })
b3:kill(db.actor)
mock.ticks(2)
check(core.quest_status("mod_a.kc_comm") == "completed", "second bandit completes the community quest")
-- section filter
local dog = mock.add_npc("dog_npc", nil, { community = "monster" })
dog:kill(db.actor)
mock.ticks(2)
check(kc_n("kc_sect") == 0, "section filter ignores another section")
local boss = mock.add_npc("boss_npc", nil, { community = "monolith" })
boss:kill(db.actor)
mock.ticks(2)
check(core.quest_status("mod_a.kc_sect") == "completed", "section filter counted the matching NPC")
-- E022: at most one filter
local function kc_codes(params)
	local q = xms_nq_load.load_asset("mod_x", "inline.nqasset", kc_quest("x", params))
	local set = {}
	for _, p in ipairs(q.problems) do set[p.code] = (set[p.code] or 0) + 1 end
	return set
end
check(kc_codes("{ count = 3 }").E022 == nil, "no filter is fine")
check(kc_codes([[{ community = "bandit" }]]).E022 == nil, "one filter is fine")
check(kc_codes([[{ community = "bandit", section = "x" }]]).E022 == 1, "two filters -> E022")
check(kc_codes([[{ community = "bandit", squad = { story = "sq" }, section = "x" }]]).E022 == 1, "three filters -> one E022")
check(kc_codes("{ count = 0 }").E006 == 1, "count keeps its min=1 -> E006")
if (failed > 0) then fail_dump() end

-- ============================================================================ (x2) kill_count on a squad
section("(x2) objective.kill_count on a quest squad, offline deaths, objective.kill unchanged")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/kc_squad.nqasset"] = [[return { nq = 1, id = "kc_squad", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "make" } },
	{ id = "make", kind = "flow.step", on_enter = { { kind = "spawn.squad", params = { section = "simulation_boar", smart = "esc_smart_terrain_2_12", ref = "gang" } } }, out = { next = "k" } },
	{ id = "k", kind = "objective.kill_count", params = { count = 2, by_actor = false, squad = { ref = "gang" } }, out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.overrides["mod_a/k_plain.nqasset"] = [[return { nq = 1, id = "k_plain", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "k" } },
	{ id = "k", kind = "objective.kill", params = { target = { spawn = { section = "simulation_boar", smart = "esc_smart_terrain_3_16", ref = "herd" } } }, out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.first_update()
core = xms_nq
qs = core.quest_state("mod_a.kc_squad")
local gang = qs.refs.gang and mock.se[qs.refs.gang.id]
check(gang ~= nil and #gang._members == 2, "the quest spawned the squad it watches")
check(qs.tokens.k ~= nil and qs.tokens.k.w.sid == gang.id, "kill_count pinned that squad down")
check(keys(qs.tokens.k.w.mem) == 2, "both members are on the watch list (" .. keys(qs.tokens.k.w.mem) .. ")")
local outsider = mock.add_npc("stalker_npc", nil, { community = "stalker" })
outsider:kill(db.actor)
mock.ticks(2)
check(tok_of("mod_a.kc_squad", "k").w.n == 0, "a death outside the squad is filtered out")
mock.kill_member(gang, gang._members[1], wolf)
mock.ticks(2)
check(tok_of("mod_a.kc_squad", "k").w.n == 1, "an online squad death counts once, killer or no killer")
mock.member_die_offline(gang, gang._members[1])
mock.ticks(2)
check(core.quest_status("mod_a.kc_squad") == "completed", "the offline death with no callback was found by the poll")
-- objective.kill on a spawned squad is untouched by any of this
local herd = core.quest_state("mod_a.k_plain").refs.herd
check(herd ~= nil and tok_of("mod_a.k_plain", "k") ~= nil, "objective.kill spawned and waits on its own squad")
mock.squad_die_offline(mock.se[herd.id])
mock.ticks(2)
check(core.quest_status("mod_a.k_plain") == "completed", "objective.kill still completes when its squad is gone")
if (failed > 0) then fail_dump() end

-- ============================================================================ (y) place{ref} and object.remove
-- A quest can now point a place at a thing it created itself, and take it back when
-- the stage is over. Before this, spawn.object could put a restrictor in the world and
-- nothing could aim at it or remove it again.
section("(y) place{ref}: reach a quest-spawned restrictor, then object.remove takes it back")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/zone.nqasset"] = [[return { nq = 1, id = "zone", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "make" } },
	{ id = "make", kind = "flow.step",
	  on_enter = { { kind = "spawn.object", params = { section = "space_restrictor", place = { level = "l01_escape", pos = { 50, 0, 50 } }, ref = "gate" } } },
	  out = { next = "reach" } },
	{ id = "reach", kind = "objective.reach", params = { place = { ref = "gate", radius = 8 }, map_spot = false }, out = { done = "clean" } },
	{ id = "clean", kind = "flow.step",
	  on_enter = { { kind = "object.remove", params = { target = { ref = "gate" } } } },
	  out = { next = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.first_update()
core = xms_nq
local UZ = "mod_a.zone"
qs = core.quest_state(UZ)
local gate = qs.refs.gate and qs.refs.gate.id
check(gate ~= nil and mock.se[gate] ~= nil, "spawn.object created the restrictor and remembered it as a ref")
check(qs.tokens.reach ~= nil, "objective.reach waits on a place that names that ref")
mock.move_actor(300, 0, 300)
mock.ticks(2)
check(core.quest_state(UZ).tokens.reach ~= nil, "standing far away does not satisfy it")
mock.move_actor(52, 0, 51)
mock.ticks(2)
check(core.quest_status(UZ) == "completed", "walking into the quest-made zone completes the objective")
check(mock.se[gate] == nil, "object.remove took the restrictor back out of the world")
check(core.quest_state(UZ).refs.gate == nil, "and cleared the ref, so nothing can resolve a dead id")
if (failed > 0) then fail_dump() end

-- ============================================================================ (z) task sub-objectives
-- One PDA task with several steps, each with its own text and its own map spot. Before
-- this, "collect X, collect Y, talk to Z" needed a whole quest per step.
section("(z) task objectives: own spots, own states, the task outlives them, saved")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/steps.nqasset"] = [[return { nq = 1, id = "steps",
	tasks = {
		gather = {
			title = "Собрать снаряжение", type = "storyline",
			objectives = {
				{ id = "bread",  title = "Найти хлеб",  target = { story = "wolf" } },
				{ id = "medkit", title = "Найти аптечку" },
				{ id = "talk",   title = "Доложить" },
			},
		},
	},
	nodes = {
		{ id = "start", kind = "trigger.start", on_enter = { { kind = "task.give", params = { task = "gather" } } }, out = { next = "w" } },
		{ id = "w", kind = "wait.when", cond = { { kind = "has_item", params = { section = "bread" } } }, out = { done = "one" } },
		{ id = "one", kind = "flow.step",
		  on_enter = { { kind = "task.objective_complete", params = { task = "gather", objective = "bread" } },
		               { kind = "task.set_objective_target", params = { task = "gather", objective = "medkit", target = { story = "wolf" } } } },
		  out = { next = "w2" } },
		{ id = "w2", kind = "wait.when",
		  cond = { { kind = "objective_status", params = { task = "gather", objective = "bread", is = "completed" } },
		           { kind = "has_item", params = { section = "medkit" } } },
		  out = { done = "two" } },
		{ id = "two", kind = "flow.step",
		  on_enter = { { kind = "task.objective_complete", params = { task = "gather", objective = "medkit" } },
		               { kind = "task.set_objective_text", params = { task = "gather", objective = "talk", new_title = "Вернуться к Волку" } } },
		  out = { next = "fin" } },
		{ id = "fin", kind = "flow.end" },
	},
} ]]
local wolf_se = mock.new_se("stalker", nil, 1, 1, nil)
mock.sor:register(wolf_se.id, "wolf")
mock.first_update()
core = xms_nq
local US = "mod_a.steps"
local t = mock.task_by_id("nq." .. US .. ".gather")
check(t ~= nil and t:get_objectives_cnt() == 3, "the task went into the PDA carrying its three steps")
check(t:get_objective(1) ~= nil and t:get_objective(1):get_title() == mock.cp("Найти хлеб"), "step one kept its own title")
check(t:get_objective(1):get_map_object_id() == wolf_se.id, "and its own map spot, separate from the task's")
check(t:get_objective(2):get_map_object_id() == 65535, "a step with no target has no spot")
qs = core.quest_state(US)
check(qs.objectives.gather.bread == "active" and qs.objectives.gather.talk == "active", "every step starts active")
mock.add_item("bread", true)
mock.ticks(2)
check(qs.objectives.gather.bread == "completed", "the first step is completed on its own")
check(t:get_objective(1):get_state() == task.completed, "and the engine objective says so")
check(t:get_state() == task.in_progress, "while the task itself stays in progress")
check(t:get_objective(2):get_map_object_id() == wolf_se.id, "set_objective_target moved the second step's spot")
mock.add_item("medkit", true)
mock.ticks(2)
check(qs.objectives.gather.medkit == "completed", "objective_status let the graph wait on step one before step two")
check(t:get_objective(3):get_title() == mock.cp("Вернуться к Волку"), "set_objective_text rewrote the third step")
check(qs.objectives.gather.talk == "active", "the untouched step is still active")
-- the steps survive a save like every other piece of quest state
mock.rebuild()
mock.first_update()
core = xms_nq
qs = core.quest_state(US)
check(qs.objectives and qs.objectives.gather and qs.objectives.gather.bread == "completed",
	"step states came back after a save/load")
local t2 = mock.task_by_id("nq." .. US .. ".gather")
check(t2 ~= nil and t2:get_objectives_cnt() == 3, "and the PDA task still carries its three steps")
if (failed > 0) then fail_dump() end

-- ============================================================================ (z2) restrictors by name
-- The name the author picks is the name the scene shows. The game does not always have
-- it under that name: a module's objects are composed in as "<module>.<op>", and
-- db.zone_by_name only holds restrictors that are online at all.
section("(z2) place{restrictor}: offline, and under the name the composer gave it")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/gate.nqasset"] = [[return { nq = 1, id = "gate",
	tasks = { go = { title = "Дойти", target = { restrictor = "esc_test_point" } } },
	nodes = {
		{ id = "start", kind = "trigger.start", on_enter = { { kind = "task.give", params = { task = "go" } } }, out = { next = "r" } },
		{ id = "r", kind = "objective.reach", params = { place = { restrictor = "esc_test_point", radius = 6 }, map_spot = false }, out = { done = "fin" } },
		{ id = "fin", kind = "flow.end" },
	},
} ]]
-- named the way xms_spawn_composer names a module's own object, and still offline
local gate = mock.add_restrictor("mod_a.esc_test_point", vector():set(40, 0, 40), false)
mock.first_update()
core = xms_nq
local UG = "mod_a.gate"
local t = mock.task_by_id("nq." .. UG .. ".go")
check(t ~= nil and t:get_map_object_id() == gate.id,
	"the task marker found the restrictor offline, under its composed name")
check(core.quest_state(UG).tokens.r ~= nil, "objective.reach is waiting")
mock.move_actor(300, 0, 300)
mock.ticks(2)
check(core.quest_state(UG).tokens.r ~= nil, "standing far away does not satisfy it")
mock.move_actor(42, 0, 41)
mock.ticks(2)
check(core.quest_status(UG) == "completed", "walking in completes it with no zone object at all")
-- and an online zone still answers through its own shape
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/gate2.nqasset"] = [[return { nq = 1, id = "gate2", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "r" } },
	{ id = "r", kind = "objective.reach", params = { place = { restrictor = "esc_ring" }, map_spot = false }, out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.add_restrictor("esc_ring", vector():set(0, 0, 0), true, 4)
mock.move_actor(50, 0, 50)		-- the actor starts at the origin, which is inside it
mock.first_update()
core = xms_nq
mock.ticks(2)
check(core.quest_state("mod_a.gate2").tokens.r ~= nil, "outside the online zone it keeps waiting")
mock.move_actor(1, 0, 1)
mock.ticks(2)
check(core.quest_status("mod_a.gate2") == "completed", "inside its shape it completes")
if (failed > 0) then fail_dump() end

-- ============================================================================ (z3) clearing a marker that is gone
-- A trigger's falling edge routinely lands after the task it watched was completed.
-- Clearing a marker then is not a mistake - the marker went with the task.
section("(z3) task.set_target with no target on a finished task is quiet")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/late.nqasset"] = [[return { nq = 1, id = "late",
	tasks = { job = { title = "Работа" } },
	nodes = {
		{ id = "start", kind = "trigger.start",
		  on_enter = { { kind = "task.give", params = { task = "job" } },
		               { kind = "task.complete", params = { task = "job" } },
		               { kind = "task.set_target", params = { task = "job" } } },
		  out = { next = "fin" } },
		{ id = "fin", kind = "flow.end" },
	},
} ]]
mock.first_update()
core = xms_nq
local UL = "mod_a.late"
check(core.quest_status(UL) == "completed", "the quest ran through instead of erroring")
check(not mock.log_has("set_target"), "and said nothing about the cleared marker")
-- naming a target the task cannot have is still an authoring mistake
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/late2.nqasset"] = [[return { nq = 1, id = "late2",
	tasks = { job = { title = "Работа" } },
	nodes = {
		{ id = "start", kind = "trigger.start",
		  on_enter = { { kind = "task.give", params = { task = "job" } },
		               { kind = "task.complete", params = { task = "job" } },
		               { kind = "task.set_target", params = { task = "job", target = { story = "wolf" } } } },
		  out = { next = "fin" } },
		{ id = "fin", kind = "flow.end" },
	},
} ]]
mock.first_update()
core = xms_nq
-- the error record is cleared when the token leaves the node, so the log is what
-- proves it was raised
check(mock.log_has("set_target"), "pointing a finished task at a target is still reported")
if (failed > 0) then fail_dump() end

-- ============================================================================ (z4) spawn where I say, stay where I say
-- A squad is created on a smart because that is what gives it something to do, but the
-- author wants it standing somewhere they chose and not wandering off the map.
section("(z4) spawn.squad: place puts them there, restrictor keeps them there")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/ambush.nqasset"] = [[return { nq = 1, id = "ambush", nodes = {
	{ id = "start", kind = "trigger.start",
	  on_enter = { { kind = "spawn.squad", params = {
	      section = "simulation_boar", smart = "esc_smart_terrain_2_12",
	      place = { level = "l01_escape", pos = { 70, 1, 70 } },
	      restrictor = "esc_ring", ref = "ambush" } } },
	  out = { next = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
local ring = mock.add_restrictor("esc_ring", vector():set(70, 0, 70), false)
mock.first_update()
core = xms_nq
local sq = core.quest_state("mod_a.ambush").refs.ambush
check(sq ~= nil, "the squad was created")
local squad = mock.se[sq.id]
local placed, confined = 0, 0
for k in squad:squad_members() do
	local se = k.object or mock.se[k.id]
	if (se and se.position and se.position:distance_to(vector():set(70, 1, 70)) < 0.01) then placed = placed + 1 end
	if (se and se._in_restr) then
		for _, z in ipairs(se._in_restr) do if (z == ring.id) then confined = confined + 1 end end
	end
end
check(placed > 0, "its members stand on the place the quest picked, not on the smart")
check(confined == placed, "and every one of them is confined to the restrictor")

-- scattered inside a restrictor, each on its own navmesh point, and spread keeps
-- them off the edges
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/ring.nqasset"] = [[return { nq = 1, id = "ring", nodes = {
	{ id = "start", kind = "trigger.start",
	  on_enter = { { kind = "spawn.squad", params = {
	      section = "simulation_boar", smart = "esc_smart_terrain_2_12",
	      place = { restrictor = "esc_ring", radius = 20 },
	      spread = 0.5, ref = "ring" } } },
	  out = { next = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.add_restrictor("esc_ring", vector():set(100, 0, 100), false)
mock.first_update()
core = xms_nq
local rs = core.quest_state("mod_a.ring").refs.ring
local rsq = rs and mock.se[rs.id]
local seen, far, n = {}, 0, 0
if (rsq) then
	for k in rsq:squad_members() do
		local se = k.object or mock.se[k.id]
		if (se) then
			n = n + 1
			seen[string.format("%d:%d", se.position.x, se.position.z)] = true
			local dx, dz = se.position.x - 100, se.position.z - 100
			if (math.sqrt(dx * dx + dz * dz) > 10.5) then far = far + 1 end
		end
	end
end
check(n > 0, "the scattered squad has members")
check(far == 0, "spread 0.5 of radius 20 keeps every member within ten metres of the centre")
local distinct = 0
for _ in pairs(seen) do distinct = distinct + 1 end
check(distinct > 0, "and each member got its own navmesh point")

-- no smart named at all: the nearest one to the place stands in
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/nosmart.nqasset"] = [[return { nq = 1, id = "nosmart", nodes = {
	{ id = "start", kind = "trigger.start",
	  on_enter = { { kind = "spawn.squad", params = {
	      section = "simulation_boar",
	      place = { level = "l01_escape", pos = { 5, 0, 5 } }, ref = "here" } } },
	  out = { next = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.first_update()
core = xms_nq
check(core.quest_state("mod_a.nosmart").refs.here ~= nil,
	"a spawn with a place and no smart still created its squad")
if (failed > 0) then fail_dump() end

-- the member ids of a squad, taken before any of them dies
function util_se_squad(id)
	local out = {}
	local se = mock.se[id]
	if not (se) then return out end
	for k in se:squad_members() do out[#out + 1] = k.id end
	return out
end

-- ============================================================================ (z5) hidden steps
-- A step can be declared hidden and shown later: it runs either way, it just takes no room
-- in the PDA until the quest says so. And because the engine saves a task without its steps,
-- a load has to put them back exactly as the player last saw them.
section("(z5) objective visibility: declared hidden, shown by the quest, and restored after a load")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.overrides["mod_a/hidden.nqasset"] = [[return { nq = 1, id = "hidden",
	tasks = {
		job = {
			title = "Работа", type = "additional",
			objectives = {
				{ id = "open",   title = "Первый шаг" },
				{ id = "secret", title = "Второй шаг", visible = false },
			},
		},
	},
	nodes = {
		{ id = "start", kind = "trigger.start", on_enter = { { kind = "task.give", params = { task = "job" } } }, out = { next = "w" } },
		{ id = "w", kind = "wait.when", cond = { { kind = "has_item", params = { section = "bread" } } }, out = { done = "show" } },
		{ id = "show", kind = "flow.step",
		  on_enter = { { kind = "task.objective_complete", params = { task = "job", objective = "open" } },
		               { kind = "task.set_objective_visible", params = { task = "job", objective = "secret", visible = true } } },
		  out = { next = "hold" } },
		{ id = "hold", kind = "wait.when", cond = { { kind = "has_item", params = { section = "vodka" } } }, out = { done = "fin" } },
		{ id = "fin", kind = "flow.end" },
	},
} ]]
mock.first_update()
core = xms_nq
local UH = "mod_a.hidden"
local th = mock.task_by_id("nq." .. UH .. ".job")
check(th ~= nil and th:get_objectives_cnt() == 2, "both steps went into the task")
check(th:get_objective(1):is_visible(), "the plain step is visible")
check(not th:get_objective(2):is_visible(), "the one declared visible = false is hidden")
mock.add_item("bread", true)
mock.ticks(2)
check(th:get_objective(2):is_visible(), "task.set_objective_visible showed it")
qs = core.quest_state(UH)
check(qs.ovis and qs.ovis.job and qs.ovis.job.secret == true, "and the runtime remembers what it did")

-- the load: the engine hands the task back with no steps at all
mock.rebuild()
mock.drop_objectives("nq." .. UH .. ".job")
mock.first_update()
core = xms_nq
local th2 = mock.task_by_id("nq." .. UH .. ".job")
check(th2 ~= nil and th2:get_objectives_cnt() == 2, "the steps were rebuilt from the declaration")
check(th2:get_objective(1):get_state() == task.completed, "the finished step came back finished")
check(th2:get_objective(2):is_visible(), "and the one the quest revealed is still visible")
check(th2:get_active_objective() == 2, "the PDA points at the first step still running")
if (failed > 0) then fail_dump() end

-- ============================================================================ (z6) an empty smart is no smart
-- The picker writes "" when nobody touched it. That is not a smart terrain named badly, it
-- is no smart terrain at all, and the place has to carry the spawn on its own.
section("(z6) spawn.squad: smart = \"\" is treated as unset, not as a smart called nothing")
setup()
for _, f in ipairs({ "linear_fetch", "dialog_branching", "parallel_triggers" }) do
	mock.deleted["mod_a/" .. f .. ".nqasset"] = true
end
mock.add_restrictor("mod_a.esc_fight", vector():set(60, 0, 60), true, 15)
-- the shape the editor writes for "spawn soldiers to kill in this restrictor"
mock.overrides["mod_a/blank.nqasset"] = [[return { nq = 1, id = "blank", nodes = {
	{ id = "start", kind = "trigger.start", out = { next = "k" } },
	{ id = "k", kind = "objective.kill", params = { target = { spawn = {
	      section = "army_sim_squad_novice", smart = "",
	      place = { restrictor = "esc_fight" }, restrictor = "esc_fight", ref = "fight" } } },
	  out = { done = "fin" } },
	{ id = "fin", kind = "flow.end" },
} }]]
mock.first_update()
core = xms_nq
qs = core.quest_state("mod_a.blank")
check(qs.refs.fight ~= nil, "the squad spawned with an empty smart and a place to stand in")
check(not mock.log_has("no smart terrain"), "and nothing complained about a missing smart")
-- a target the quest spawned is findable only if something marks it
sid = qs.refs.fight and qs.refs.fight.id
check(sid ~= nil and mock.spots[sid .. "|secondary_task_location"] ~= nil,
	"objective.kill put a marker on the squad it spawned")
sq_fight = util_se_squad(sid)
for _, mid in ipairs(sq_fight) do mock.kill_member(mock.se[sid], mid, db.actor) end
mock.ticks(2)
check(mock.spots[sid .. "|secondary_task_location"] == nil, "and took it off once they were dead")
check(core.quest_status("mod_a.blank") == "completed", "killing them finished the objective")
if (failed > 0) then fail_dump() end

-- ============================================================================ summary
io.write(string.format("\n%d passed, %d failed\n", passed, failed))
if (failed > 0) then
	fail_dump()
	error("tests failed", 0)
end
