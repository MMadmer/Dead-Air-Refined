#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C"
{
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

int luaopen_marshal(lua_State* state);
}

namespace
{
using Clock = std::chrono::steady_clock;

struct IncrementalResult
{
    std::vector<char> bytes;
    std::size_t encodeCalls{};
    std::size_t readCalls{};
    double totalMilliseconds{};
    double maximumCallMilliseconds{};
    double encodeMilliseconds{};
    double readMilliseconds{};
    double maximumEncodeCallMilliseconds{};
    double maximumReadCallMilliseconds{};
};

std::vector<char> read_file(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("cannot open input file: " + path);

    const auto length = stream.tellg();
    std::vector<char> data(static_cast<std::size_t>(length));
    stream.seekg(0);
    stream.read(data.data(), length);
    return data;
}

void check_lua(lua_State* state, int status, const char* operation)
{
    if (!status)
        return;

    const char* error = lua_tostring(state, -1);
    throw std::runtime_error(std::string(operation) + ": " + (error ? error : "unknown Lua error"));
}

void push_marshal_function(lua_State* state, const char* name)
{
    lua_getglobal(state, "marshal");
    lua_getfield(state, -1, name);
    lua_remove(state, -2);
}

std::vector<char> encode_once(lua_State* state, int valueReference, int constantsReference = LUA_NOREF)
{
    push_marshal_function(state, "encode");
    lua_rawgeti(state, LUA_REGISTRYINDEX, valueReference);
    int argumentCount = 1;
    if (constantsReference >= 0)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, constantsReference);
        argumentCount = 2;
    }
    check_lua(state, lua_pcall(state, argumentCount, 1, 0), "marshal.encode");

    std::size_t size = 0;
    const char* data = lua_tolstring(state, -1, &size);
    if (!data)
        throw std::runtime_error("marshal.encode did not return a string");
    std::vector<char> result(data, data + size);
    lua_pop(state, 1);
    return result;
}

IncrementalResult encode_incremental(
    lua_State* state,
    int valueReference,
    int constantsReference,
    int operationBudget)
{
    push_marshal_function(state, "encode_begin");
    lua_rawgeti(state, LUA_REGISTRYINDEX, valueReference);
    int argumentCount = 1;
    if (constantsReference >= 0)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, constantsReference);
        argumentCount = 2;
    }
    check_lua(state, lua_pcall(state, argumentCount, 1, 0), "marshal.encode_begin");
    const int encoderReference = luaL_ref(state, LUA_REGISTRYINDEX);

    IncrementalResult result;
    bool completed = false;
    while (!completed)
    {
        push_marshal_function(state, "encode_step");
        lua_rawgeti(state, LUA_REGISTRYINDEX, encoderReference);
        lua_pushinteger(state, operationBudget);
        const auto begin = Clock::now();
        check_lua(state, lua_pcall(state, 2, 1, 0), "marshal.encode_step");
        const auto end = Clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(end - begin).count();
        result.totalMilliseconds += elapsed;
        result.maximumCallMilliseconds = std::max(result.maximumCallMilliseconds, elapsed);
        result.encodeMilliseconds += elapsed;
        result.maximumEncodeCallMilliseconds = std::max(result.maximumEncodeCallMilliseconds, elapsed);
        ++result.encodeCalls;

        completed = lua_toboolean(state, -1) != 0;
        lua_pop(state, 1);
    }

    push_marshal_function(state, "encode_size");
    lua_rawgeti(state, LUA_REGISTRYINDEX, encoderReference);
    check_lua(state, lua_pcall(state, 1, 1, 0), "marshal.encode_size");
    result.bytes.reserve(static_cast<std::size_t>(lua_tonumber(state, -1)));
    lua_pop(state, 1);

    bool readCompleted = false;
    while (!readCompleted)
    {
        push_marshal_function(state, "encode_read");
        lua_rawgeti(state, LUA_REGISTRYINDEX, encoderReference);
        lua_pushinteger(state, 64 * 1024);
        const auto begin = Clock::now();
        check_lua(state, lua_pcall(state, 2, 2, 0), "marshal.encode_read");
        const auto end = Clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(end - begin).count();
        result.totalMilliseconds += elapsed;
        result.maximumCallMilliseconds = std::max(result.maximumCallMilliseconds, elapsed);
        result.readMilliseconds += elapsed;
        result.maximumReadCallMilliseconds = std::max(result.maximumReadCallMilliseconds, elapsed);
        ++result.readCalls;

        readCompleted = lua_toboolean(state, -2) != 0;
        std::size_t size = 0;
        const char* data = lua_tolstring(state, -1, &size);
        if (!data)
            throw std::runtime_error("marshal.encode_read did not return a string");
        result.bytes.insert(result.bytes.end(), data, data + size);
        lua_pop(state, 2);
    }

    push_marshal_function(state, "encode_read");
    lua_rawgeti(state, LUA_REGISTRYINDEX, encoderReference);
    lua_pushinteger(state, 64 * 1024);
    check_lua(state, lua_pcall(state, 2, 2, 0), "marshal.encode_read after EOF");
    std::size_t eofSize = 0;
    const char* eofData = lua_tolstring(state, -1, &eofSize);
    if (!lua_toboolean(state, -2) || !eofData || eofSize)
        throw std::runtime_error("marshal.encode_read after EOF did not return an empty terminal chunk");
    lua_pop(state, 2);

    luaL_unref(state, LUA_REGISTRYINDEX, encoderReference);
    lua_gc(state, LUA_GCCOLLECT, 0);
    return result;
}

void install_ctime_fixture(lua_State* state)
{
    static constexpr char source[] = R"(
        local values = setmetatable({}, { __mode = "k" })
        local methods = {}
        function methods:set(year, month, day, hour, minute, second, millisecond)
            values[self] = {year, month, day, hour, minute, second, millisecond}
        end
        local function persist(self)
            local parts = values[self] or {0, 0, 0, 0, 0, 0, 0}
            return string.format(
                "local value=game.CTime();value:set(%d,%d,%d,%d,%d,%d,%d);return value",
                unpack(parts)
            )
        end
        game = {}
        function game.CTime()
            local value = newproxy(true)
            local metatable = getmetatable(value)
            metatable.__index = methods
            metatable.__persist = persist
            values[value] = {0, 0, 0, 0, 0, 0, 0}
            return value
        end
    )";
    check_lua(state, luaL_dostring(state, source), "install CTime fixture");
}

int decode(lua_State* state, const std::vector<char>& encoded, int constantsReference = LUA_NOREF)
{
    push_marshal_function(state, "decode");
    lua_pushlstring(state, encoded.data(), encoded.size());
    int argumentCount = 1;
    if (constantsReference >= 0)
    {
        lua_rawgeti(state, LUA_REGISTRYINDEX, constantsReference);
        argumentCount = 2;
    }
    check_lua(state, lua_pcall(state, argumentCount, 1, 0), "marshal.decode");
    return luaL_ref(state, LUA_REGISTRYINDEX);
}

int build_constants(lua_State* state)
{
    lua_newtable(state);
    lua_getglobal(state, "print");
    lua_rawseti(state, -2, 1);
    return luaL_ref(state, LUA_REGISTRYINDEX);
}

int build_special_fixture(lua_State* state)
{
    static constexpr char source[] = R"(
        local shared = {answer = 42, flags = {true, false, true}}
        local persisted = setmetatable({value = 73}, {
            __persist = function(self)
                local value = self.value
                return function() return {value = value} end
            end
        })
        local time = game.CTime()
        time:set(2026, 8, 5, 12, 34, 56, 789)
        local upvalue = shared
        special_fixture = {
            first = shared,
            second = shared,
            persisted = persisted,
            time = time,
            same_time = time,
            callback = function() return upvalue end,
            constant = print
        }
        special_fixture.self = special_fixture
        special_fixture[shared] = shared.flags
        special_fixture[special_fixture.callback] = time
    )";
    check_lua(state, luaL_dostring(state, source), "build special fixture");
    lua_getglobal(state, "special_fixture");
    return luaL_ref(state, LUA_REGISTRYINDEX);
}

void validate_special_fixture(lua_State* state, int fixtureReference)
{
    lua_rawgeti(state, LUA_REGISTRYINDEX, fixtureReference);
    const int root = lua_gettop(state);
    lua_getfield(state, root, "self");
    if (!lua_rawequal(state, root, -1))
        throw std::runtime_error("self reference was not preserved");
    lua_pop(state, 1);

    lua_getfield(state, root, "first");
    lua_getfield(state, root, "second");
    if (!lua_rawequal(state, -1, -2))
        throw std::runtime_error("shared table reference was not preserved");
    lua_pop(state, 2);

    lua_getfield(state, root, "callback");
    check_lua(state, lua_pcall(state, 0, 1, 0), "call decoded callback");
    lua_getfield(state, root, "first");
    if (!lua_rawequal(state, -1, -2))
        throw std::runtime_error("function upvalue reference was not preserved");
    lua_pop(state, 2);

    lua_getfield(state, root, "constant");
    lua_getglobal(state, "print");
    if (!lua_rawequal(state, -1, -2))
        throw std::runtime_error("constant function reference was not preserved");
    lua_pop(state, 2);

    lua_getfield(state, root, "first");
    lua_rawget(state, root);
    lua_getfield(state, root, "first");
    lua_getfield(state, -1, "flags");
    lua_remove(state, -2);
    if (!lua_rawequal(state, -1, -2))
        throw std::runtime_error("complex table key was not preserved");
    lua_pop(state, 2);

    lua_getfield(state, root, "callback");
    lua_rawget(state, root);
    lua_getfield(state, root, "time");
    if (!lua_rawequal(state, -1, -2))
        throw std::runtime_error("complex function key was not preserved");
    lua_pop(state, 2);

    lua_getfield(state, root, "time");
    lua_getfield(state, root, "same_time");
    if (!lua_rawequal(state, -1, -2))
        throw std::runtime_error("shared userdata reference was not preserved");
    lua_pop(state, 3);
}

void validate_abandoned_encoder_gc(lua_State* state)
{
    static constexpr char source[] = R"(
        local weak = setmetatable({}, {__mode = "v"})
        do
            local root = {}
            root.self = root
            weak[1] = root
            local encoder = marshal.encode_begin(root)
            assert(not marshal.encode_step(encoder, 1))
            root = nil
            encoder = nil
        end
        for _ = 1, 4 do collectgarbage("collect") end
        assert(weak[1] == nil)
    )";
    check_lua(state, luaL_dostring(state, source), "validate abandoned encoder GC");
    std::cout << "abandoned_encoder_gc=pass\n";
}

void validate_failed_encoder_state(lua_State* state)
{
    static constexpr char source[] = R"(
        local value = setmetatable({}, {
            __persist = function() error("intentional encoder failure") end
        })
        local encoder = marshal.encode_begin(value)
        assert(not pcall(marshal.encode_step, encoder, 1))
        local ok, message = pcall(marshal.encode_step, encoder, 1)
        assert(not ok and tostring(message):find("previous step", 1, true))
    )";
    check_lua(state, luaL_dostring(state, source), "validate failed encoder state");
    std::cout << "failed_encoder_state=pass\n";
}

void validate_table_persist_references(lua_State* state)
{
    static constexpr char source[] = R"(
        local shared = {answer = 42}
        local persisted = setmetatable({value = 73}, {
            __persist = function(self)
                local value = self.value
                return function() return {value = value} end
            end
        })
        local root = {persisted, shared, shared}
        local encoded = marshal.encode(root)
        local decoded = marshal.decode(encoded)
        assert(decoded[1].value == 73)
        assert(decoded[2] == decoded[3])

        local encoder = marshal.encode_begin(root)
        while not marshal.encode_step(encoder, 1) do end
        local chunks = {}
        while true do
            local done, chunk = marshal.encode_read(encoder, 7)
            chunks[#chunks + 1] = chunk
            if done then break end
        end
        local incremental = table.concat(chunks)
        assert(incremental == encoded)
        decoded = marshal.decode(incremental)
        assert(decoded[1].value == 73)
        assert(decoded[2] == decoded[3])
    )";
    check_lua(state, luaL_dostring(state, source), "validate table persist references");
    std::cout << "table_persist_references=pass\n";
}

void require_decode_failure(lua_State* state, const std::vector<char>& encoded, std::size_t length)
{
    push_marshal_function(state, "decode");
    lua_pushlstring(state, encoded.data(), length);
    if (!lua_pcall(state, 1, 1, 0))
    {
        lua_pop(state, 1);
        throw std::runtime_error("marshal.decode accepted truncated input");
    }
    lua_pop(state, 1);
}

void validate_truncated_decode(lua_State* state, const std::vector<char>& encoded)
{
    for (std::size_t length = 0; length < encoded.size(); ++length)
        require_decode_failure(state, encoded, length);
    std::cout << "truncated_decode=pass cases=" << encoded.size() << '\n';
}

int build_large_fixture(lua_State* state)
{
    static constexpr char source[] = R"(
        large_fixture = {}
        for index = 1, 40000 do
            large_fixture[index] = {
                id = index,
                enabled = index % 3 == 0,
                name = "persistent_object_" .. index,
                position = {index * 0.25, index * 0.5, index * 0.75}
            }
        end
    )";
    check_lua(state, luaL_dostring(state, source), "build large fixture");
    lua_getglobal(state, "large_fixture");
    return luaL_ref(state, LUA_REGISTRYINDEX);
}

void require_equal(const std::vector<char>& expected, const std::vector<char>& actual, const char* label)
{
    if (expected != actual)
        throw std::runtime_error(std::string(label) + " byte stream differs from marshal.encode");
}

void validate_capture_script(lua_State* state, const char* scriptPath)
{
    lua_pushstring(state, scriptPath);
    lua_setglobal(state, "qa_capture_script_path");
    static constexpr char source[] = R"(
        USE_MARSHAL = true
        GAME_VERSION = "marshal-incremental-qa"
        db = {storage = {}}
        function is_empty(value) return next(value) == nil end
        function SendScriptCallback() end
        function printf() end
        assert(loadfile(qa_capture_script_path))()

        assert(CALifeStorageManager_capture_prepare_begin("qa.scop"))
        while not CALifeStorageManager_capture_prepare_step(64) do end
        local expected = assert(CALifeStorageManager_capture_encode())
        assert(CALifeStorageManager_capture_encode_begin())
        local chunks = {}
        local expected_size
        while true do
            local status = CALifeStorageManager_capture_encode_step(7)
            assert(status >= 0 and status <= 2)
            if status > 0 then
                expected_size = expected_size or CALifeStorageManager_capture_encode_size()
                chunks[#chunks + 1] = assert(CALifeStorageManager_capture_encode_result())
            end
            if status == 2 then break end
        end
        local actual = table.concat(chunks)
        assert(expected_size == #actual)
        assert(expected == actual)

        USE_MARSHAL = false
        assert(CALifeStorageManager_capture_prepare_begin("qa-disabled.scop"))
        assert(CALifeStorageManager_capture_encode_begin())
        assert(CALifeStorageManager_capture_encode_step(7) == 2)
        assert(CALifeStorageManager_capture_encode_size() == 0)
        assert(CALifeStorageManager_capture_encode_result() == nil)
    )";
    check_lua(state, luaL_dostring(state, source), "validate ALife capture script");
    std::cout << "capture_script_wiring=pass\n";
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 3)
        {
            std::cerr << "usage: marshal_incremental_test <legacy.scoc> <alife_storage_manager.script>\n";
            return 2;
        }

        lua_State* state = luaL_newstate();
        if (!state)
            throw std::runtime_error("cannot create Lua state");
        luaL_openlibs(state);
        luaopen_marshal(state);
        lua_setglobal(state, "marshal");
        install_ctime_fixture(state);
        validate_capture_script(state, argv[2]);

        const auto legacyBytes = read_file(argv[1]);
        const int legacyReference = decode(state, legacyBytes);
        std::cout << "legacy_decode=pass\n";
        const auto legacyBaseline = encode_once(state, legacyReference);
        std::cout << "legacy_input_bytes=" << legacyBytes.size()
                  << " reencoded_bytes=" << legacyBaseline.size()
                  << " monolithic_reencode_input_exact=" << (legacyBytes == legacyBaseline ? 1 : 0) << '\n';

        for (const int budget : {1, 7, 64, 257})
        {
            const auto encoded = encode_incremental(state, legacyReference, LUA_NOREF, budget);
            require_equal(legacyBaseline, encoded.bytes, "legacy incremental encoding");
            std::cout << "legacy budget=" << budget << " encode_calls=" << encoded.encodeCalls
                      << " read_calls=" << encoded.readCalls
                      << " exact_vs_monolithic=1"
                      << " total_ms=" << encoded.totalMilliseconds
                      << " max_call_ms=" << encoded.maximumCallMilliseconds
                      << " encode_ms=" << encoded.encodeMilliseconds
                      << " read_ms=" << encoded.readMilliseconds
                      << " max_encode_call_ms=" << encoded.maximumEncodeCallMilliseconds
                      << " max_read_call_ms=" << encoded.maximumReadCallMilliseconds << '\n';
        }

        const int constantsReference = build_constants(state);
        const int specialReference = build_special_fixture(state);
        const auto specialBaseline = encode_once(state, specialReference, constantsReference);
        for (const int budget : {1, 3, 32})
        {
            const auto encoded = encode_incremental(state, specialReference, constantsReference, budget);
            require_equal(specialBaseline, encoded.bytes, "special incremental encoding");
            const int decodedReference = decode(state, encoded.bytes, constantsReference);
            validate_special_fixture(state, decodedReference);
            luaL_unref(state, LUA_REGISTRYINDEX, decodedReference);
        }
        std::cout << "special_semantics=pass exact_vs_monolithic=1 bytes=" << specialBaseline.size() << '\n';
        validate_table_persist_references(state);
        validate_truncated_decode(state, specialBaseline);
        validate_abandoned_encoder_gc(state);
        validate_failed_encoder_state(state);

        const int largeReference = build_large_fixture(state);
        const auto largeIncremental = encode_incremental(state, largeReference, LUA_NOREF, 256);
        const auto monolithicBegin = Clock::now();
        const auto largeBaseline = encode_once(state, largeReference);
        const auto monolithicEnd = Clock::now();
        const double monolithicMilliseconds =
            std::chrono::duration<double, std::milli>(monolithicEnd - monolithicBegin).count();
        require_equal(largeBaseline, largeIncremental.bytes, "large incremental encoding");
        std::cout << "large bytes=" << largeBaseline.size()
                  << " exact_vs_monolithic=1"
                  << " monolithic_ms=" << monolithicMilliseconds
                  << " incremental_encode_calls=" << largeIncremental.encodeCalls
                  << " incremental_read_calls=" << largeIncremental.readCalls
                  << " incremental_total_ms=" << largeIncremental.totalMilliseconds
                  << " incremental_max_call_ms=" << largeIncremental.maximumCallMilliseconds
                  << " incremental_encode_ms=" << largeIncremental.encodeMilliseconds
                  << " incremental_read_ms=" << largeIncremental.readMilliseconds
                  << " incremental_max_encode_call_ms=" << largeIncremental.maximumEncodeCallMilliseconds
                  << " incremental_max_read_call_ms=" << largeIncremental.maximumReadCallMilliseconds << '\n';

        luaL_unref(state, LUA_REGISTRYINDEX, largeReference);
        luaL_unref(state, LUA_REGISTRYINDEX, specialReference);
        luaL_unref(state, LUA_REGISTRYINDEX, constantsReference);
        luaL_unref(state, LUA_REGISTRYINDEX, legacyReference);
        lua_close(state);
        std::cout << "marshal_incremental_qa=pass\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
