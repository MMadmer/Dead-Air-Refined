local target_name = "compat_encode_false"

local function normalize_save_name(value)
    local name = tostring(value):lower():gsub("\\", "/")
    name = name:match("([^/]+)$") or name
    return name:gsub("%.scop$", "")
end

return function()
    local console = get_console()
    local storage = alife_storage_manager
    local encode_called = false
    local unexpected_callback = false

    local function marker(...)
        local fields = { ... }
        log1(table.concat(fields, " "))
        flush1()
    end

    storage.CALifeStorageManager_capture_prepare_begin = function(fname)
        marker("QA_SERIALIZER_COMPAT_ENCODE_BEGIN", normalize_save_name(fname))
        return true
    end
    storage.CALifeStorageManager_capture_prepare_step = function(item_budget)
        return true
    end
    storage.CALifeStorageManager_capture_encode_begin = nil
    storage.CALifeStorageManager_capture_encode_step = nil
    storage.CALifeStorageManager_capture_encode_result = nil
    storage.CALifeStorageManager_capture_encode_size = nil
    storage.CALifeStorageManager_capture_encode = function()
        encode_called = true
        marker("QA_SERIALIZER_COMPAT_ENCODE_FALSE")
        return false
    end
    storage.CALifeStorageManager_capture_prepare = nil
    storage.CALifeStorageManager_capture_save = nil
    storage.CALifeStorageManager_before_save = function(fname)
        marker("QA_SERIALIZER_COMPAT_ERROR", "unexpected_legacy_fallback=" .. normalize_save_name(fname))
        unexpected_callback = true
    end

    local original_save_callback = storage.CALifeStorageManager_save
    storage.CALifeStorageManager_save = function(fname)
        if type(original_save_callback) == "function" then
            original_save_callback(fname)
        end

        if normalize_save_name(fname) == target_name then
            marker("QA_SERIALIZER_COMPAT_ERROR", "unexpected_commit_callback=" .. target_name)
            unexpected_callback = true
        end
    end

    console:execute("time_factor 0")
    for frame = 1, 240 do
        coroutine.yield()
    end

    console:execute("save " .. target_name)
    marker("QA_SERIALIZER_COMPAT_ENCODE_REQUESTED", target_name)

    for frame = 1, 1200 do
        if unexpected_callback then
            console:execute("quit")
            return
        end
        coroutine.yield()
    end

    if not encode_called then
        marker("QA_SERIALIZER_COMPAT_ERROR", "encode_not_called")
    else
        marker("QA_SERIALIZER_COMPAT_ENCODE_FALSE_DONE")
    end
    console:execute("quit")
end
