-- The aim zoom overlay reaches the weapons and what inherits from them: a section the overlay names,
-- a child that does not say the key itself, a scope variant with its own line.
--
--   tools\qa\Run-WaterQa.ps1 -Name aim-zoom -Scenario aim_zoom -Save qa_meadow -KeepShaderCache
local done = false
local checks = {
	{ "wpn_ak74", "scope_zoom_factor", 1.1 },
	{ "wpn_ak74_kobra", "scope_zoom_factor", 1.1 },
	{ "wpn_svd", "scope_zoom_factor", 1.1 },
	{ "wpn_9a91_ps01", "scope_zoom_factor", 4.0 },
}
local function on_update()
	if done or not db.actor then return end
	done = true
	local ini, passed = system_ini(), 0
	for _, c in ipairs(checks) do
		local v = ini:section_exist(c[1]) and ini:r_float_ex(c[1], c[2]) or nil
		local ok = v ~= nil and math.abs(v - c[3]) < 1e-4
		if ok then passed = passed + 1 end
		printf("[qa] %s.%s = %s (want %s) %s", c[1], c[2], tostring(v), tostring(c[3]), ok and "PASS" or "FAIL")
	end
	-- the engine reads on/yes/true/1 as a true bool (CInifile::r_bool)
	local dyn = ini:r_string_ex("wpn_9a91_ps01", "scope_dynamic_zoom")
	local ok = dyn == "on"
	if ok then passed = passed + 1 end
	printf("[qa] wpn_9a91_ps01.scope_dynamic_zoom = %s %s", tostring(dyn), ok and "PASS" or "FAIL")
	printf(string.format("[qa] DONE %d/%d", passed, #checks + 1))
	printf("DA_WATER_PROBE_DONE")
	get_console():execute("flush")
end
function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
