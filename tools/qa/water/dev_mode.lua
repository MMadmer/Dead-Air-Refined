-- The debug mode gate (dar_debug_menu): the console's key opens nothing with it off and the console with it on. The
-- runner presses the key on "[qa] dev READY <n>" (Send-HiddenDesktopKey.ps1, VK_OEM_3) and captures the window
-- (Capture-HiddenDesktopWindow.ps1) before "[qa] dev <step> looked".
--
--   tools\qa\Run-WaterQa.ps1 -Name dev-mode -Scenario dev_mode -Save qa_meadow -KeepShaderCache -Seconds 60
local stage, t0, frames, step = "start", 0, 0, 0
local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local plan = { "dar_debug_menu 0", "dar_debug_menu 1" }

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 120 then return end
	local now = time_global()
	if stage == "start" then
		stage, t0 = "next", now
	elseif stage == "next" and now - t0 > 1500 then
		step = step + 1
		if not plan[step] then
			say("[qa] dev DONE")
			say("DA_WATER_PROBE_DONE")
			stage = "end"
			return
		end
		cmd(plan[step])
		say("[qa] dev READY " .. step)
		stage, t0 = "pressed", now
	elseif stage == "pressed" and now - t0 > 4000 then
		say("[qa] dev " .. plan[step] .. " looked")
		stage, t0 = "next", now
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
