local target_name = "compat_malformed_scov"

return function()
    local console = get_console()

    local function marker(value)
        log1(value)
        flush1()
    end

    console:execute("time_factor 0")
    for frame = 1, 240 do
        coroutine.yield()
    end

    local original_actor = db.actor
    if not original_actor then
        marker("QA_SERIALIZER_COMPAT_ERROR missing_actor_before_load")
        console:execute("quit")
        return
    end
    local original_level = level.name()
    local original_actor_id = original_actor:id()

    marker("QA_SERIALIZER_COMPAT_MALFORMED_REQUESTED")
    console:execute("load " .. target_name)

    for frame = 1, 600 do
        coroutine.yield()
    end

    local current_actor = db.actor
    if not current_actor or level.name() ~= original_level or current_actor:id() ~= original_actor_id then
        marker("QA_SERIALIZER_COMPAT_ERROR active_world_changed")
        console:execute("quit")
        return
    end

    marker("QA_SERIALIZER_COMPAT_MALFORMED_SURVIVED " .. original_level .. " " .. original_actor_id)
    console:execute("quit")
end
