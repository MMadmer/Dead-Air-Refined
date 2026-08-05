local cases = {
    {
        name = "compat_legacy_noop_existing",
        operation = "noop",
        previous = "legacy-noop-existing:v1"
    },
    {
        name = "compat_legacy_noop_absent",
        operation = "noop",
        previous = false
    },
    {
        name = "compat_legacy_append_existing",
        operation = "append",
        previous = "legacy-append-existing:v1",
        payload = "|lua-append-existing"
    },
    {
        name = "compat_legacy_append_absent",
        operation = "append",
        previous = false,
        payload = "lua-append-absent"
    },
    {
        name = "compat_legacy_delete_existing",
        operation = "delete",
        previous = "legacy-delete-existing:v1"
    },
    {
        name = "compat_legacy_delete_absent",
        operation = "delete",
        previous = false
    },
    {
        name = "compat_legacy_create_existing",
        operation = "create",
        previous = "legacy-create-existing:old",
        payload = "legacy-create-existing:new"
    },
    {
        name = "compat_legacy_create_absent",
        operation = "create",
        previous = false,
        payload = "legacy-create-absent:new"
    },
    {
        name = "compat_legacy_zero_existing",
        operation = "zero",
        previous = "legacy-zero-existing:old"
    },
    {
        name = "compat_legacy_zero_absent",
        operation = "zero",
        previous = false
    }
}

local case_by_name = {}
for _, case in ipairs(cases) do
    case_by_name[case.name] = case
end

local function normalize_save_name(value)
    local name = tostring(value):lower():gsub("\\", "/")
    name = name:match("([^/]+)$") or name
    return name:gsub("%.scop$", "")
end

local function read_binary(path)
    local file = io.open(path, "rb")
    if io.type(file) ~= "file" then
        return nil
    end

    local data = file:read("*all")
    file:close()
    return data
end

local function write_binary(path, data, mode)
    local file = io.open(path, mode or "wb")
    if io.type(file) ~= "file" then
        return false
    end

    file:write(data)
    file:flush()
    file:close()
    return true
end

return function()
    local console = get_console()
    local storage = alife_storage_manager
    local saves_path = getFS():update_path("$game_saves$", "")
    local failure
    local completed = {}

    local function marker(...)
        local fields = { ... }
        log1(table.concat(fields, " "))
        flush1()
    end

    local function fail(message)
        if not failure then
            failure = tostring(message)
            marker("QA_SERIALIZER_COMPAT_ERROR", failure)
        end
    end

    storage.CALifeStorageManager_capture_prepare_begin = nil
    storage.CALifeStorageManager_capture_prepare_step = nil
    storage.CALifeStorageManager_capture_prepare = nil
    storage.CALifeStorageManager_capture_encode = nil
    storage.CALifeStorageManager_capture_save = nil

    storage.CALifeStorageManager_before_save = function(fname)
        local name = normalize_save_name(fname)
        local case = case_by_name[name]
        if not case then
            fail("unexpected_legacy_callback=" .. name)
            return
        end

        local path = saves_path .. name .. ".scoc"
        local previous = read_binary(path)
        if case.previous == false then
            if previous ~= nil then
                fail("unexpected_previous_scoc=" .. name)
                return
            end
        elseif previous ~= case.previous then
            fail("previous_scoc_mismatch=" .. name)
            return
        end

        if case.operation == "append" then
            if not write_binary(path, case.payload, "ab") then
                fail("append_failed=" .. name)
                return
            end
        elseif case.operation == "delete" then
            if previous ~= nil then
                getFS():file_delete("$game_saves$", name .. ".scoc")
            end
        elseif case.operation == "create" then
            if not write_binary(path, case.payload) then
                fail("create_failed=" .. name)
                return
            end
        elseif case.operation == "zero" then
            if not write_binary(path, "") then
                fail("zero_failed=" .. name)
                return
            end
        elseif case.operation ~= "noop" then
            fail("unknown_operation=" .. tostring(case.operation))
            return
        end

        marker("QA_SERIALIZER_COMPAT_LEGACY_CALLBACK", name, case.operation)
    end

    local original_save_callback = storage.CALifeStorageManager_save
    storage.CALifeStorageManager_save = function(fname)
        if type(original_save_callback) == "function" then
            original_save_callback(fname)
        end

        local name = normalize_save_name(fname)
        if case_by_name[name] then
            completed[name] = true
            marker("QA_SERIALIZER_COMPAT_LEGACY_COMMITTED", name)
        end
    end

    console:execute("time_factor 0")
    for frame = 1, 240 do
        coroutine.yield()
    end

    marker("QA_SERIALIZER_COMPAT_LEGACY_BEGIN")
    for _, case in ipairs(cases) do
        console:execute("save " .. case.name)
        marker("QA_SERIALIZER_COMPAT_LEGACY_REQUESTED", case.name)

        local timed_out = true
        for frame = 1, 1800 do
            if failure or completed[case.name] then
                timed_out = false
                break
            end
            coroutine.yield()
        end

        if timed_out then
            fail("save_timeout=" .. case.name)
        end
        if failure then
            console:execute("quit")
            return
        end
    end

    marker("QA_SERIALIZER_COMPAT_LEGACY_DONE")
    console:execute("quit")
end
