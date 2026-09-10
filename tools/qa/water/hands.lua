-- Hands that never come back. The animation module claims the player's hands and his input the
-- moment an item use is intercepted, and gives them back from a time event that only exists once
-- the scene has actually STARTED - so anything that ends the wait without starting the scene left
-- the player able to walk and look and nothing else, for good.
--
-- Two things are asserted here, in the real game, through the real functions:
--   1. THE WATCHDOG. Claim the hands the way a scene does and never start one. Everything must
--      come back on its own within a frame or two.
--   2. THE DOUBLE ANIMATION. While a scene is running, the base mod's own use animation must not
--      play on top of it.
-- `qa_hands_state` goes to the log around each step, so a failure shows which gate was still up.
local stage, t0, frames = 0, 0, 0
local passed, total = 0, 0

local function cmd(s) get_console():execute(s) end

local function check(step, ok, detail)
	total = total + 1
	if (ok) then passed = passed + 1 end
	printf("HANDSQA: %s %s %s", step, ok and "PASS" or "FAIL", tostring(detail or ""))
	cmd("flush")
end

local function gates_up()
	return game.only_movekeys_allowed()
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 60 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		check("module-loaded", enhanced_animations ~= nil and enhanced_animations.anim_prepare ~= nil,
			"enhanced_animations")
		check("gates-clear-at-rest", not gates_up(), "only_movekeys_allowed")
		cmd("qa_hands_state")
		t0 = now stage = 1

	elseif stage == 1 and after(500) then
		-- A scene claims the hands and the input, and then never starts: exactly what happens
		-- when the wait is ended by something other than the scene beginning.
		enhanced_animations.anim_prepare()
		check("gates-taken", gates_up(), "only_movekeys_allowed after anim_prepare")
		cmd("qa_hands_state")
		t0 = now stage = 2

	elseif stage == 2 and after(2000) then
		-- The watchdog runs once a frame; two seconds is a thousand chances.
		check("watchdog-released-input", not gates_up(), "only_movekeys_allowed")
		cmd("qa_hands_state")
		t0 = now stage = 3

	elseif stage == 3 and after(500) then
		check("slot-usable-again", db.actor:active_slot() ~= nil,
			"active_slot " .. tostring(db.actor:active_slot()))
		-- The addon's own copy of this feature must stay out of it while ours has a scene. Its
		-- use_item sets dar2_animations_item.anim_state to "full" almost first thing and holsters
		-- the weapon before that, so an untouched state is the honest witness that it never ran.
		check("dar2-patch-installed",
			dar2_animations_enhanced == nil or dar2_animations_enhanced.da_use_item_original ~= nil,
			"da_use_item_original")
		if (dar2_animations_enhanced and dar2_animations_item) then
			dar2_animations_item.anim_state = "none"
			enhanced_animations.used_item = "item_ea_bandage"
			dar2_animations_enhanced.use_item(nil, "bandage")
			check("dar2-scene-suppressed", dar2_animations_item.anim_state == "none",
				"anim_state " .. tostring(dar2_animations_item.anim_state))
			enhanced_animations.used_item = nil
		else
			check("dar2-scene-suppressed", true, "addon not installed")
		end
		t0 = now stage = 4

	elseif stage == 4 and after(1500) then
		check("gates-clear-at-end", not gates_up(), "only_movekeys_allowed")
		cmd("qa_hands_state")
		printf(string.format("HANDSQA: DONE %d/%d", passed, total))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 5
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
