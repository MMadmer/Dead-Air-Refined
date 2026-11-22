-- A world save made without a module that changes the class of a section of the game, loaded with it
-- (docs/dead-air/SAVE_COMPATIBILITY.md, "A class a module changes"): the save's objects of that section were written by
-- the game's class. They load, as objects of the module's class, and the next save writes them into the module's chunk
-- of .scov, a placeholder holding each id in .scop.
--   qa_cc_ids = "27899,23603,17888"   object ids to report (section, class id, parent)
--   qa_cc_save = "qa_classchange"     the save written after the load
--
--   tools\qa\Run-WaterQa.ps1 -Name cc -Scenario class_change_save -Save "admin - quicksave1" -KeepShaderCache -Seconds 90
local stage, t0, frames = 0, 0, 0

local function cmd(s) get_console():execute(s) end
local function say(s) printf(s) cmd("flush") end

local function on_update()
	if not db.actor then return end
	device():pause(false)
	frames = frames + 1
	if frames < 90 then return end
	local now = time_global()

	if stage == 0 then
		say("[qa] cc level " .. level.name())
		for id in string.gmatch(qa_cc_ids or "", "[0-9]+") do
			local se = alife():object(tonumber(id))
			if se then
				say(string.format("[qa] cc object %s section %s clsid %s parent %s", id, se:section_name(),
					tostring(se:clsid()), tostring(se.parent_id)))
			else
				say("[qa] cc object " .. id .. " missing")
			end
		end
		cmd("save " .. (qa_cc_save or "qa_classchange"))
		t0 = now
		stage = 1
	elseif stage == 1 and now - t0 > 6000 then
		local name = qa_cc_save or "qa_classchange"
		say("[qa] cc save-written " .. tostring(getFS():exist("$game_saves$", name .. ".scop") ~= nil))
		say("[qa] cc DONE")
		say("DA_WATER_PROBE_DONE")
		stage = 2
	end
end

function on_game_start()
	RegisterScriptCallback("actor_on_update", on_update)
end
