-- The hands rig changes under whatever is in the hands: an outfit brings its own, the 3D PDA
-- brings its own. The rigs do not carry the same motion sets - the PDA's is trimmed - and an item
-- that kept its motion ids from the previous rig indexes a model that no longer exists. That is
-- the crash a player sent in (access violation in xrGame, right after "hands swap in").
--
-- This does the swap directly, with a weapon drawn, and asks the item to animate on the far side
-- of it: draw and holster while the trimmed rig is loaded, then swap back and do it again. A run
-- that reaches DONE is a run where nothing played an id the rig does not have.
local stage, t0, frames, round = 0, 0, 0, 0
local rounds = 3
local other_rig = "pda3d_actor_hud"
local function cmd(s) get_console():execute(s) end

local function report(tag)
	printf(string.format("RIGQA: %s round %d slot %s", tag, round, tostring(db.actor:active_slot())))
	cmd("qa_hands_state")
	cmd("flush")
end

local function on_update()
	if not db.actor then return end
	frames = frames + 1
	if frames < 60 then return end
	local now = time_global()
	local function after(ms) return now - t0 > ms end

	if stage == 0 then
		db.actor:activate_slot(2)
		report("start")
		t0 = now stage = 1

	elseif stage == 1 and after(1500) then
		-- Pool an item whose cycles come from an OMF appended LATE to this rig - a high motion
		-- slot, past the end of the trimmed one. That is the id the player's crash carried
		-- (slot 64 against a model with 44), and asking for its length is the crash itself.
		round = round + 1
		cmd("qa_hud_motion pda_show_animator_hud anm_show")
		cmd("qa_hud_motion wpn_hand_hammer_hud anm_idle")
		cmd("qa_hands_rig " .. other_rig)
		-- The same two items on the other rig. Rebound, each answers with what THIS rig has -
		-- and an item whose cycles the trimmed rig does not carry answers with nothing at all.
		-- Holding the old ids, they answer with whatever those indices mean here instead.
		cmd("qa_hud_motion pda_show_animator_hud anm_show")
		cmd("qa_hud_motion wpn_hand_hammer_hud anm_idle")
		report("swapped-in")
		t0 = now stage = 2

	elseif stage == 2 and after(1000) then
		-- Ask for animations while the trimmed rig is loaded: holster and draw both go through
		-- the item's own cycles, which is where the dangling ids were read.
		db.actor:activate_slot(0)
		t0 = now stage = 3

	elseif stage == 3 and after(1200) then
		db.actor:activate_slot(2)
		report("animated-on-other-rig")
		t0 = now stage = 4

	elseif stage == 4 and after(1500) then
		cmd("qa_hands_rig actor_hud_cs1")
		cmd("qa_hud_motion pda_show_animator_hud anm_show")
		report("swapped-out")
		t0 = now stage = 5

	elseif stage == 5 and after(1200) then
		-- And the cycles must be back: the same draw, on the rig that owns them.
		db.actor:activate_slot(0)
		t0 = now stage = 6

	elseif stage == 6 and after(1200) then
		db.actor:activate_slot(2)
		report("back")
		t0 = now
		stage = (round < rounds) and 1 or 7

	elseif stage == 7 and after(1500) then
		report("end")
		printf(string.format("RIGQA: DONE %d round(s)", round))
		printf("DA_WATER_PROBE_DONE")
		cmd("flush")
		stage = 8
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
