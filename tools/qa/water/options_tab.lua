-- Options -> Video, Game, Sound and Controls, the weather dialog, the bolt's icon and the addon's
-- harvest copy, in the real game.
--
--   tools\qa\Run-WaterQa.ps1 -Name options -Scenario options_tab -Save <any> -KeepShaderCache
--
-- Asserted: the legacy form set_actor_position(pos, lvid, gvid) moves the actor; the static hint
-- export round-trips; the animation module has scenes for the addon's leftover items; the addon's
-- harvest copy is skipped while the module owns the harvest; the Video tab is the grouped list -
-- every subsection, the renderer row, a hint on every row, a number beside every slider, and the
-- anti-aliasing rules (an upscaler greys out AA, AMD FSR and NVIDIA DLSS switch each other off)
-- without leaving anything changed; the Game tab has no rows it should not have and a hint for
-- every row; Sound and Controls are grouped lists like Video - their subsections, a hint on every
-- row, a number beside every slider, closecaption.script's subtitle controls moved into rows, and
-- on Controls the key list as rows of the same list: a sample of actions with their hint
-- (<caption id>_hint), both key cells, and the key a binding has; the main menu's weather dialog
-- (atmosfear_options.script) is two such lists - a row per level with its good and bad weather, a row
-- per event setting - each row hinted, each control the dialog's own and showing the saved value.
-- Frames: all four tabs from top to bottom, Sound and Controls once more with the cursor resting on a
-- row (its hint shows after 0.7 s), the weather dialog's pages and a hint on each, the bolt in the
-- hands and in the backpack.
local frames = 0
local passed, total = 0, 0
local dlg

local function cmd(s) get_console():execute(s) end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	printf("[qa] %s %s %s", step, ok and "PASS" or "FAIL", tostring(detail or ""))
	cmd("flush")
end

-- The frame is taken from outside, window and all (the dialog is UI, which the engine's own
-- capture leaves out): tools\qa\Watch-QaShots.ps1 grabs the window on this marker.
local shots = 0
local function shot()
	shots = shots + 1
	printf(string.format("[qa] SHOT %02d", shots))
	cmd("flush")
end

-- The scenario: each step runs once the previous one is this many ms old.
local steps = {}
local function step(wait, fn) steps[#steps + 1] = { wait = wait, fn = fn } end

-- A list from top to bottom: four frames, at 0, 1/3, 2/3 and the end of its scroll range.
local function shoot_list(get_list)
	step(800, shot)
	for _, part in ipairs({ 1 / 3, 2 / 3 }) do
		step(1300, function()
			local sv = get_list()
			sv:SetScrollPos(math.floor(sv:GetMinScrollPos() + (sv:GetMaxScrollPos() - sv:GetMinScrollPos()) * part))
		end)
		step(300, shot)
	end
	step(1300, function() get_list():ScrollToEnd() end)
	step(300, shot)
end

step(0, function()
	-- The legacy form must land where the plain one does (the collision correction moves both).
	local p = db.actor:position()
	db.actor:set_actor_position(vector():set(p.x, p.y + 3, p.z))
	local dy1 = db.actor:position().y - p.y
	db.actor:set_actor_position(p)
	db.actor:set_actor_position(vector():set(p.x, p.y + 3, p.z), 1, 1)
	local dy3 = db.actor:position().y - p.y
	db.actor:set_actor_position(p)
	check("legacy-set_actor_position", dy3 > 1 and math.abs(dy3 - dy1) < 0.05, string.format("plain dy=%.2f legacy dy=%.2f", dy1, dy3))

	local st = CUIStatic()
	st:SetHint("ui_mm_tips_hint")
	check("static-hint-export", st:GetHint() == "ui_mm_tips_hint", tostring(st:GetHint()))

	for _, s in ipairs({ "jgut", "drug_lsd_3", "drug_iodide", "bottle_1_metal_2", "mutant_part_rat_corpse", "bread" }) do
		check("module-scene-" .. s, enhanced_animations.da_has_anim(s) == true, "")
	end

	if (dar2_animations_enhanced) then
		-- Unskipped, the copy takes the monster first thing (used_monster), or dies on its
		-- config when it was never activated; skipped, it touches nothing.
		dar2_animations_enhanced.used_monster_2 = db.actor
		dar2_animations_enhanced.used_monster = nil
		local ok, err = pcall(dar2_animations_enhanced.monster_on_actor_use_2)
		check("addon-harvest-skipped", ok and dar2_animations_enhanced.used_monster == nil,
			string.format("ok=%s used_monster=%s err=%s", tostring(ok), tostring(dar2_animations_enhanced.used_monster), tostring(err)))
		dar2_animations_enhanced.used_monster_2 = nil
		dar2_animations_enhanced.used_monster = nil
	end
end)

step(500, function()
	-- The main menu registers closecaption.script's option callbacks when it opens (its subtitle
	-- controls go on the sound page); the probe builds the dialog without the menu.
	if (closecaption and closecaption.main_menu_on_init) then closecaption.main_menu_on_init() end
	dlg = ui_mm_opt_main.options_dialog()
	dlg:SetCurrentValues()
	dlg:ShowDialog(true)
	dlg.tab:SetActiveTab("video")
	dlg:OnTabChange()

	local page = dlg.dlg_video
	local caps = get_console():get_string("r__caps")
	printf("[qa] r__caps " .. tostring(caps))
	printf(string.format("[qa] video groups (%d): %s", #page.headers, table.concat(page.headers, " ")))
	printf(string.format("[qa] video rows (%d): %s", #page.order, table.concat(page.order, " ")))

	-- Every subsection shows; DLSS only on a card that has it, the rest always.
	check("video-groups", table.concat(page.headers, " ") == "screen quality aa rtx image effects color",
		table.concat(page.headers, " "))
	local expected = { "renderer", "async_compute", "resolution", "window_mode", "fps_limit", "vsync", "preset",
		"level_texture_unload", "dstorage", "aa", "fsr", "framegen", "upscale_sharpness", "rt_reflections", "rt_shadows",
		"rtao", "rt_quality", "gamma", "contrast", "brightness", "fov", "hud_fov", "detail_height", "sun_shafts", "sharp",
		"bloom", "aberration", "dof", "technicolor", "vignette", "color_r", "color_g", "color_b", "vibrance" }
	local missing = {}
	for _, id in ipairs(expected) do
		if not (page.rows[id]) then missing[#missing + 1] = id end
	end
	check("video-rows", #missing == 0, table.concat(missing, " "))
	check("video-dlss-row-follows-caps", (page.rows.dlss ~= nil) == (string.find(caps or "", "dlss=1", 1, true) ~= nil), tostring(page.rows.dlss ~= nil))
	check("video-renderer-shown", dlg.combo_renderer == (page.rows.renderer and page.rows.renderer.ctrl) and dlg.combo_renderer:IsShown(), "")

	local without_hint = {}
	for _, id in ipairs(page.order) do
		if (page.rows[id].hint == "") then without_hint[#without_hint + 1] = id end
	end
	check("video-rows-have-hints", #without_hint == 0, table.concat(without_hint, " "))

	-- The RTX category: its own subsection with the four ray tracing rows in order, right after
	-- anti-aliasing and upscaling.
	local rtx = {}
	for _, id in ipairs(page.order) do
		if (page.rows[id].group == "rtx") then rtx[#rtx + 1] = id end
	end
	check("video-rtx-rows", table.concat(rtx, " ") == "rt_reflections rt_shadows rtao rt_quality", table.concat(rtx, " "))

	-- The DirectX 12 rows greyed with the reason the capabilities give (PLAN.md 2.6).
	local c = {}
	for key, value in string.gmatch(caps or "", "([%w_]+)%s*=%s*([^;%s]+)") do
		c[key] = tonumber(value) or 0
	end
	local function cap(key) return c[key] or 0 end
	local function expect(id, off_hint)
		local row = page.rows[id]
		if not (row) then
			check("video-state-" .. id, false, "no row")
			return
		end
		local enabled = row.ctrl:IsEnabled()
		local ok = (off_hint == nil and enabled) or (off_hint ~= nil and not enabled and row.shown_hint == off_hint)
		check("video-state-" .. id, ok, string.format("enabled=%s hint=%s expected=%s", tostring(enabled),
			tostring(row.shown_hint), tostring(off_hint or "enabled")))
	end
	local renderer = page.rows.renderer
	local dx12_chosen = renderer == nil or renderer.ctrl:CurrentID() == 7
	local h = "st_da_x64_video_hint_"
	local function dx12_state(dx11_hint, available, unavailable_hint)
		if (cap("api") ~= 12) then return dx11_hint end
		if not (available) then return unavailable_hint end
		if not (dx12_chosen) then return dx11_hint end
		return nil
	end
	page:Refresh()
	expect("async_compute", dx12_state(h .. "async_compute_dx11", cap("acq") == 1, h .. "async_compute_unsupported"))
	expect("dstorage", dx12_state(h .. "dstorage_dx11", cap("dstorage") == 1, h .. "dstorage_unavailable"))
	local rt_dx11 = (cap("rt_tier") >= 11) and (h .. "rt_dx11") or (h .. "rt_dx11_nohw")
	for _, id in ipairs({ "rt_reflections", "rt_shadows", "rtao" }) do
		expect(id, dx12_state(rt_dx11, cap("rt") == 1, h .. "rt_unsupported"))
	end
	local quality = dx12_state(rt_dx11, cap("rt") == 1, h .. "rt_unsupported")
	if (quality == nil and not page:AnyRtEffect()) then quality = h .. "rt_quality_off" end
	expect("rt_quality", quality)

	page:Refresh()
	local empty = 0
	for _, readout in ipairs(page.readouts) do
		if (readout.wnd:TextControl():GetText() == "") then empty = empty + 1 end
	end
	check("video-readouts-filled", #page.readouts > 0 and empty == 0, string.format("%d of %d empty, hud_fov=%s", empty, #page.readouts,
		tostring(page.hud_fov_value and page.hud_fov_value:TextControl():GetText())))

	-- The live rules, then everything read back from the console: nothing is accepted here.
	local rows = page.rows
	if (rows.fsr) then
		rows.fsr.ctrl:SetCurrentID(1)
		page:OnUpscalerSelected("fsr")
		check("video-fsr-greys-aa", rows.aa ~= nil and not rows.aa.ctrl:IsEnabled() and
			(rows.upscale_sharpness == nil or rows.upscale_sharpness.ctrl:IsEnabled()), tostring(rows.aa and rows.aa.shown_hint))
		if (rows.dlss) then
			check("video-fsr-switches-dlss-off", rows.dlss.ctrl:CurrentID() == 0, tostring(rows.dlss.ctrl:CurrentID()))
			rows.dlss.ctrl:SetCurrentID(1)
			page:OnUpscalerSelected("dlss")
			check("video-dlss-switches-fsr-off", rows.fsr.ctrl:CurrentID() == 0, tostring(rows.fsr.ctrl:CurrentID()))
		end
	end
	COptionsManager():SetCurrentValues("mm_opt_video")
	page:Refresh()
	local upscaling = page:IsOn("fsr") or page:IsOn("dlss")
	check("video-values-read-back", rows.aa == nil or rows.aa.ctrl:IsEnabled() == not upscaling,
		string.format("fsr=%s dlss=%s", tostring(get_console():get_string("r__fsr")), tostring(get_console():get_string("r__dlss"))))

	page.list:ScrollToBegin()
end)
shoot_list(function() return dlg.dlg_video.list end)

step(1300, function()
	dlg.tab:SetActiveTab("gameplay")
	dlg:OnTabChange()

	local rows, without_ctrl, without_hint = {}, {}, {}
	for k, v in pairs(dlg.gameplay_options) do
		if not (v.disabled) then
			rows[#rows + 1] = k
			if not (v.ctrl) then without_ctrl[#without_ctrl + 1] = k end
			local id = v.hint or ("ui_mm_" .. k .. "_hint")
			if (game.translate_string(id) == id) then without_hint[#without_hint + 1] = k end
		end
	end
	table.sort(rows)
	printf(string.format("[qa] rows (%d): %s", #rows, table.concat(rows, " ")))
	check("rows-have-controls", #without_ctrl == 0, table.concat(without_ctrl, " "))
	check("rows-have-hints", #without_hint == 0, table.concat(without_hint, " "))
	for _, k in ipairs({ "npc_steal_box", "consumable_animation", "localization" }) do
		check("row-dropped-" .. k, dlg.gameplay_options[k] == nil, "")
	end
	dlg.gameplay_scroll_v:ScrollToBegin()
end)
shoot_list(function() return dlg.gameplay_scroll_v end)

-- A page that is one list (dead_air_x64_option_list.script): the subsections and rows expected are
-- there, and every row of it has a hint that is in the string table.
local function check_list_page(tab, page, groups, rows)
	printf(string.format("[qa] %s groups (%d): %s", tab, #page.headers, table.concat(page.headers, " ")))
	printf(string.format("[qa] %s rows (%d): %s", tab, #page.order, table.concat(page.order, " ")))
	local missing = {}
	for _, id in ipairs(groups) do
		local found = false
		for _, header in ipairs(page.headers) do
			found = found or header == id
		end
		if not (found) then missing[#missing + 1] = "group:" .. id end
	end
	for _, id in ipairs(rows) do
		if not (page.rows[id]) then missing[#missing + 1] = id end
	end
	check(tab .. "-rows", #missing == 0, table.concat(missing, " "))

	local without_hint = {}
	for _, id in ipairs(page.order) do
		local hint = page.rows[id].hint
		if (hint == "") or (game.translate_string(hint) == hint) or (page.rows[id].wnd:GetHint() ~= hint) then
			without_hint[#without_hint + 1] = id
		end
	end
	check(tab .. "-rows-have-hints", #page.order > 0 and #without_hint == 0, table.concat(without_hint, " "))

	if (page.Refresh) then page:Refresh() end
	local empty = 0
	for _, readout in ipairs(page.readouts) do
		if (readout.wnd:TextControl():GetText() == "") then empty = empty + 1 end
	end
	check(tab .. "-readouts-filled", empty == 0, string.format("%d of %d empty", empty, #page.readouts))
end

-- Rests the cursor on the middle of a window, for the frame that shows its hint (0.7 s later).
local function hover(wnd)
	local r = Frect()
	wnd:GetAbsoluteRect(r)
	SetCursorPosition(vector2():set((r.x1 + r.x2) / 2, (r.y1 + r.y2) / 2))
end

local function hover_shot(get_wnd, tab)
	step(1300, function() hover(get_wnd()) end)
	step(1500, function()
		printf("[qa] %s hover over=%s hint=%s", tab, tostring(get_wnd():IsCursorOverWindow()), tostring(get_wnd():GetHint()))
		shot()
	end)
	-- Off the row while its page is still shown: a hidden row never loses the cursor, and the hint
	-- it holds would keep every other row's from showing.
	step(500, function() SetCursorPosition(vector2():set(0, 0)) end)
end

step(1300, function()
	dlg.tab:SetActiveTab("sound")
	dlg:OnTabChange()
	local page = dlg.dlg_sound

	-- The subtitle rows are closecaption.script's controls, moved into the list when it is there.
	local groups, rows = { "volume", "playback" }, { "master_volume", "music_volume", "snd_device", "eax", "dynamic_music" }
	if (dlg.tab_captioning) then
		groups[#groups + 1] = "subtitles"
		rows[#rows + 1] = "cc_captioning"
		rows[#rows + 1] = "cc_icc"
	end
	check_list_page("sound", page, groups, rows)
	if (dlg.tab_captioning) then
		local captioning, icc = page.rows.cc_captioning, page.rows.cc_icc
		check("sound-subtitles-adopted", captioning ~= nil and captioning.ctrl == dlg.tab_captioning and icc ~= nil
			and icc.ctrl == dlg.check_closedcaptions and not dlg.cap_captioning:IsShown(), "")
	end
	page.list:ScrollToBegin()
end)
shoot_list(function() return dlg.dlg_sound.list end)
step(1300, function() dlg.dlg_sound.list:ScrollToBegin() end)
hover_shot(function() return dlg.dlg_sound.rows.master_volume.wnd end, "sound")

step(1300, function()
	dlg.tab:SetActiveTab("controls")
	dlg:OnTabChange()
	local page = dlg.dlg_controls

	-- Its own rows, then the key list: a subsection per group of ui_keybinding.xml, a row per action
	-- with the hint <caption id>_hint (kb_map is the row the engine adds on its own).
	local keys = { "kb_forward", "kb_jump", "kb_sprint", "kb_weapon1", "kb_reload", "kb_func", "kb_inventory",
		"kb_quick_use_1", "kb_map", "kb_custom5", "kb_use", "ui_mm_save_game", "kb_console" }
	local rows = { "mouse_sense", "invert_mouse", "aim_toggle", "keyb_default" }
	for _, id in ipairs(keys) do rows[#rows + 1] = id end
	check_list_page("controls", page, { "mouse", "keys", "kb_grp_movement", "kb_grp_weapons", "kb_grp_inventory",
		"kb_grp_common" }, rows)

	local wrong = {}
	for _, id in ipairs(keys) do
		local row = page.rows[id]
		if not (row and row.hint == id .. "_hint" and row.keys and row.keys[1] and row.keys[2]) then
			wrong[#wrong + 1] = id
		end
	end
	check("keys-rows-hint-and-cells", #wrong == 0, table.concat(wrong, " "))

	-- The cells read the bindings when the dialog read its groups: moving forward has a key.
	local forward = page.rows.kb_forward
	local text = forward and forward.keys[1]:GetText() or ""
	check("keys-cells-show-bindings", text ~= "" and text ~= "---", text)
	page.list:ScrollToBegin()
end)
shoot_list(function() return dlg.dlg_controls.list end)
step(1300, function() dlg.dlg_controls.list:ScrollToBegin() end)
hover_shot(function() return dlg.dlg_controls.rows.kb_forward.wnd end, "controls")

-- The weather dialog of the main menu (atmosfear_options.script): both pages are lists with a hint on
-- every row, every control is still the dialog's own, and each shows the value the config holds.
-- Nothing is accepted.
local af
step(1300, function()
	dlg:HideDialog()
	af = atmosfear_options.af_options_dialog()
	af.owner = dlg
	af:ShowDialog(true)
	af:OnBtnWeatherOptions()

	local levels, wrong = {}, {}
	for level_name in pairs(level_weathers.valid_levels) do
		levels[#levels + 1] = level_name
	end
	local weather = af.dlg_weather
	check_list_page("weather-periods", weather, { "periods" }, levels)
	for _, level_name in ipairs(levels) do
		local row = weather.rows[level_name]
		local config = atmosfear_options.config
		for _, period in ipairs({ "good", "bad" }) do
			local name = level_name .. "_period_" .. period
			local saved = config:r_value("atmosfear_current_parameters", "opt_" .. name)
			local shown = af:GetComboValue(name)
			if not (row and af["list_" .. name]) or (saved ~= nil and tostring(shown) ~= tostring(saved)) then
				wrong[#wrong + 1] = string.format("%s=%s/%s", name, tostring(shown), tostring(saved))
			end
		end
	end
	check("weather-periods-show-config", #wrong == 0, table.concat(wrong, " "))

	local events = af.dlg_event
	check_list_page("weather-events", events, { "blowout", "psi_storm", "fallout" }, { "enable_blowout", "blowout_freq",
		"blowout_task", "enable_psi_storm", "psi_storm_freq", "psi_storm_task", "enable_fallout", "fallout_freq", "fallout_task" })
	local mismatched = {}
	for _, id in ipairs(events.order) do
		local control = af["check_" .. id] or af["list_" .. id]
		if (events.rows[id].ctrl ~= control) then mismatched[#mismatched + 1] = id end
	end
	check("weather-events-controls-are-the-dialogs", #mismatched == 0, table.concat(mismatched, " "))
	weather.list:ScrollToBegin()
end)
step(800, shot)
step(1300, function() af.dlg_weather.list:ScrollToEnd() end)
step(300, shot)
step(1300, function() af.dlg_weather.list:ScrollToBegin() end)
hover_shot(function() return af.dlg_weather.rows[af.dlg_weather.order[1]].wnd end, "weather-periods")
step(1300, function()
	af:OnBtnEventOptions()
	af.dlg_event.list:ScrollToBegin()
end)
step(800, shot)
hover_shot(function() return af.dlg_event.rows.enable_blowout.wnd end, "weather-events")
step(1300, function()
	af:HideDialog()
end)

step(1300, function()
	if (closecaption and closecaption.main_menu_on_quit) then closecaption.main_menu_on_quit() end
	cmd("g_infinite_bolts 0")
	local pos, lv, gv = db.actor:position(), db.actor:level_vertex_id(), db.actor:game_vertex_id()
	alife():create("bolt", pos, lv, gv, db.actor:id())
	alife():create("bolt", pos, lv, gv, db.actor:id())
	check("bolt-description", game.translate_string(system_ini():r_string_ex("bolt", "description") or "") ~= "",
		tostring(system_ini():r_string_ex("bolt", "description")))
	check("bolt-icons-texture", system_ini():r_string_ex("bolt", "icons_texture") ~= nil,
		tostring(system_ini():r_string_ex("bolt", "icons_texture")))
end)
step(1000, function()
	-- Whatever slot number the bolt's config maps to: find it.
	for slot = 0, 16 do
		local item = db.actor:item_in_slot(slot)
		if (item and item:section() == "bolt") then
			printf("[qa] bolt in slot " .. slot)
			db.actor:activate_slot(slot)
			break
		end
	end
end)
step(2000, shot)
step(1300, function() get_hud():ShowActorMenu() end)
step(2500, shot)
step(1300, function()
	printf(string.format("[qa] DONE %d/%d", passed, total))
	printf("DA_WATER_PROBE_DONE")
	cmd("flush")
end)

local current, t0 = 1, 0
local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 90 then return end
	local s = steps[current]
	if not (s) then return end
	local now = time_global()
	if (current == 1 or now - t0 > s.wait) then
		s.fn()
		t0 = now
		current = current + 1
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
