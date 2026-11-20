-- Player report of 1.6.3: dying with the mutant loot window open and loading the last save crashed in
-- CDialogHolder::UpdateCursorVisibility - the script window was freed with the script engine's restart while the HUD
-- still held it. Two script windows are shown: one let go by Lua and collected at once, one kept by a global through a
-- load of the same save (the quick-load path restarts the script engine). A console float marks the second pass; the
-- run has to reach the marker after the load.
--
--   tools\qa\Run-WaterQa.ps1 -Name dialog165 -Scenario dialog_165 -Save glowsq -KeepShaderCache -Seconds 90 -ExtraArgs "-r5"
local stage, t0, frames = "start", 0, 0
local mark = 0.0137
local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

class "qa_dialog_165" (CUIScriptWnd)
function qa_dialog_165:__init() super()
	self:SetWndRect(Frect():set(100, 100, 400, 300))
end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 150 then return end
	local now = time_global()

	if stage == "start" then
		if math.abs(get_console():get_float("r__mask_jitter") - mark) < 1e-5 then
			cmd("r__mask_jitter 0.005")
			say("[qa] dialog second pass: alive after the load")
			say("DA_WATER_PROBE_DONE")
			stage = "end"
			return
		end
		local dropped = qa_dialog_165()
		dropped:ShowDialog(true)
		dropped = nil
		collectgarbage("collect")
		collectgarbage("collect")
		say("[qa] dialog dropped window collected")
		stage, t0 = "kept", now
	elseif stage == "kept" and now - t0 > 2000 then
		say("[qa] dialog frames after the collect: alive")
		_G.qa_kept_dialog = qa_dialog_165()
		_G.qa_kept_dialog:ShowDialog(true)
		cmd("r__mask_jitter " .. mark)
		say("[qa] dialog loading with a window open")
		cmd("load glowsq")
		stage = "loading"
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
