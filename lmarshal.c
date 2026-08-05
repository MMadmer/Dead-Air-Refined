/*
* lmarshal.c
* A Lua library for serializing and deserializing Lua values
* Richard Hundt <richardhundt@gmail.com>
*
* License: MIT
*
* Copyright (c) 2010 Richard Hundt
*
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without
* restriction, including without limitation the rights to use,
* copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following
* conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
* HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"


#define MAR_TREF 1
#define MAR_TVAL 2
#define MAR_TUSR 3

#define MAR_CHR 1
#define MAR_I32 4
#define MAR_I64 8

#define MAR_MAGIC 0x8e
#define SEEN_IDX  3
#define ITERATOR_KEYS_IDX 4
#define ACTION_VALUES_IDX 5
#define MAR_ENCODE_STATE_METATABLE "marshal.encode_state"

typedef struct mar_Buffer {
    size_t size;
    size_t seek;
    size_t head;
    char*  data;
} mar_Buffer;

#if defined(_MSC_VER)
#define MAR_THREAD_LOCAL __declspec(thread)
#else
#define MAR_THREAD_LOCAL _Thread_local
#endif

static MAR_THREAD_LOCAL size_t mar_encode_buffer_size_hint = 128;

static int mar_encode_table(lua_State *L, mar_Buffer *buf, size_t *idx);
static int mar_decode_table(lua_State *L, const char* buf, size_t len, size_t *idx);

static void buf_init(lua_State *L, mar_Buffer *buf, size_t initial_size)
{
    buf->size = initial_size;
    buf->seek = 0;
    buf->head = 0;
    if (!(buf->data = malloc(buf->size))) luaL_error(L, "Out of memory!");
}

static void buf_done(lua_State* L, mar_Buffer *buf)
{
    free(buf->data);
}

static void buf_reserve(lua_State* L, size_t len, mar_Buffer* buf)
{
    if (len > SIZE_MAX - buf->head) luaL_error(L, "buffer too long");
    if (buf->size - buf->head < len) {
        size_t new_size = buf->size;
        size_t cur_head = buf->head;
        while (new_size - cur_head < len) {
            if (new_size > SIZE_MAX / 2) {
                new_size = cur_head + len;
                break;
            }
            new_size <<= 1;
        }
        {
            char *new_data = realloc(buf->data, new_size);
            if (!new_data) {
                luaL_error(L, "Out of memory!");
            }
            buf->data = new_data;
        }
        buf->size = new_size;
    }
}

static int buf_write(lua_State* L, const char* str, size_t len, mar_Buffer *buf)
{
    if (len > UINT32_MAX) luaL_error(L, "buffer too long");
    buf_reserve(L, len, buf);
    memcpy(&buf->data[buf->head], str, len);
    buf->head += len;
    return 0;
}

static size_t buf_begin_block(lua_State *L, mar_Buffer *buf)
{
    const uint32_t empty_length = 0;
    const size_t length_position = buf->head;
    buf_write(L, (const char*)&empty_length, MAR_I32, buf);
    return length_position;
}

static void buf_end_block(lua_State *L, mar_Buffer *buf, size_t length_position)
{
    const size_t data_position = length_position + MAR_I32;
    const size_t length = buf->head - data_position;
    uint32_t encoded_length;
    if (length > UINT32_MAX) luaL_error(L, "buffer too long");
    encoded_length = (uint32_t)length;
    memcpy(buf->data + length_position, &encoded_length, MAR_I32);
}

static const char* buf_read(lua_State *L, mar_Buffer *buf, size_t *len)
{
    if (buf->seek < buf->head) {
        buf->seek = buf->head;
        *len = buf->seek;
        return buf->data;
    }
    *len = 0;
    return NULL;
}

static void mar_encode_source_callback(
    lua_State *L,
    mar_Buffer *buf,
    const char *source,
    size_t source_len,
    size_t *idx)
{
    const unsigned char number_type = LUA_TNUMBER;
    const unsigned char function_type = LUA_TFUNCTION;
    const unsigned char value_tag = MAR_TVAL;
    const uint32_t encoded_source_len = (uint32_t)source_len;
    const uint32_t empty_upvalues_len = 0;
    const lua_Number callback_key = 1;

    if (source_len > UINT32_MAX) {
        luaL_error(L, "portable callback source is too long");
    }

    buf_write(L, (const char*)&number_type, MAR_CHR, buf);
    buf_write(L, (const char*)&callback_key, MAR_I64, buf);
    buf_write(L, (const char*)&function_type, MAR_CHR, buf);
    buf_write(L, (const char*)&value_tag, MAR_CHR, buf);
    buf_write(L, (const char*)&encoded_source_len, MAR_I32, buf);
    buf_write(L, source, source_len, buf);
    buf_write(L, (const char*)&empty_upvalues_len, MAR_I32, buf);
    (*idx)++;
}

static void mar_encode_value(lua_State *L, mar_Buffer *buf, int val, size_t *idx)
{
    size_t l;
    int val_type = lua_type(L, val);
    int pushed_value = 0;

    switch (val_type) {
    case LUA_TBOOLEAN: {
        int int_val = lua_toboolean(L, val);
        buf_reserve(L, MAR_CHR * 2, buf);
        buf->data[buf->head++] = (char)val_type;
        buf->data[buf->head++] = (char)int_val;
        break;
    }
    case LUA_TSTRING: {
        const char *str_val = lua_tolstring(L, val, &l);
        uint32_t encoded_length;
        if (l > UINT32_MAX || l > SIZE_MAX - MAR_CHR - MAR_I32) luaL_error(L, "buffer too long");
        encoded_length = (uint32_t)l;
        buf_reserve(L, MAR_CHR + MAR_I32 + l, buf);
        buf->data[buf->head++] = (char)val_type;
        memcpy(&buf->data[buf->head], &encoded_length, MAR_I32);
        buf->head += MAR_I32;
        memcpy(&buf->data[buf->head], str_val, l);
        buf->head += l;
        break;
    }
    case LUA_TNUMBER: {
        lua_Number num_val = lua_tonumber(L, val);
        buf_reserve(L, MAR_CHR + MAR_I64, buf);
        buf->data[buf->head++] = (char)val_type;
        memcpy(&buf->data[buf->head], &num_val, MAR_I64);
        buf->head += MAR_I64;
        break;
    }
    case LUA_TTABLE: {
        int tag, ref;
        buf_write(L, (void*)&val_type, MAR_CHR, buf);
        lua_pushvalue(L, val);
        pushed_value = 1;
        lua_pushvalue(L, -1);
        lua_rawget(L, SEEN_IDX);
        if (!lua_isnil(L, -1)) {
            ref = lua_tointeger(L, -1);
            tag = MAR_TREF;
            buf_write(L, (void*)&tag, MAR_CHR, buf);
            buf_write(L, (void*)&ref, MAR_I32, buf);
            lua_pop(L, 1);
        }
        else {
            lua_pop(L, 1); /* pop nil */
            if (luaL_getmetafield(L, -1, "__persist")) {
                size_t length_position;
                tag = MAR_TUSR;

                lua_pushvalue(L, -2); /* self */
                lua_call(L, 1, 1);
                if (!lua_isfunction(L, -1)) {
                    luaL_error(L, "__persist must return a function");
                }

                lua_remove(L, -2); /* __persist */

                lua_newtable(L);
                lua_pushvalue(L, -2); /* callback */
                lua_rawseti(L, -2, 1);

                buf_write(L, (void*)&tag, MAR_CHR, buf);
                length_position = buf_begin_block(L, buf);
                mar_encode_table(L, buf, idx);
                buf_end_block(L, buf, length_position);
                lua_pop(L, 1);
            }
            else {
                size_t length_position;
                tag = MAR_TVAL;

                lua_pushvalue(L, -1);
                lua_pushinteger(L, (*idx)++);
                lua_rawset(L, SEEN_IDX);

                lua_pushvalue(L, -1);
                buf_write(L, (void*)&tag, MAR_CHR, buf);
                length_position = buf_begin_block(L, buf);
                mar_encode_table(L, buf, idx);
                buf_end_block(L, buf, length_position);
                lua_pop(L, 1);
            }
        }
        break;
    }
    case LUA_TFUNCTION: {
        int tag, ref;
        buf_write(L, (void*)&val_type, MAR_CHR, buf);
        lua_pushvalue(L, val);
        pushed_value = 1;
        lua_pushvalue(L, -1);
        lua_rawget(L, SEEN_IDX);
        if (!lua_isnil(L, -1)) {
            ref = lua_tointeger(L, -1);
            tag = MAR_TREF;
            buf_write(L, (void*)&tag, MAR_CHR, buf);
            buf_write(L, (void*)&ref, MAR_I32, buf);
            lua_pop(L, 1);
        }
        else {
            int i;
            lua_Debug ar;
            size_t length_position;
            lua_pop(L, 1); /* pop nil */

            lua_pushvalue(L, -1);
            lua_getinfo(L, ">nuS", &ar);
            if (ar.what[0] != 'L') {
                luaL_error(L, "attempt to persist a C function '%s'", ar.name);
            }
            tag = MAR_TVAL;
            lua_pushvalue(L, -1);
            lua_pushinteger(L, (*idx)++);
            lua_rawset(L, SEEN_IDX);

            lua_pushvalue(L, -1);
            buf_write(L, (void*)&tag, MAR_CHR, buf);
            length_position = buf_begin_block(L, buf);
            lua_dump(L, (lua_Writer)buf_write, buf);
            buf_end_block(L, buf, length_position);
            lua_pop(L, 1);

            lua_newtable(L);
            for (i=1; i <= ar.nups; i++) {
                lua_getupvalue(L, -2, i);
                lua_rawseti(L, -2, i);
            }

            length_position = buf_begin_block(L, buf);
            mar_encode_table(L, buf, idx);
            buf_end_block(L, buf, length_position);
            lua_pop(L, 1);
        }

        break;
    }
    case LUA_TUSERDATA: {
        int tag, ref;
        buf_write(L, (void*)&val_type, MAR_CHR, buf);
        lua_pushvalue(L, val);
        pushed_value = 1;
        lua_pushvalue(L, -1);
        lua_rawget(L, SEEN_IDX);
        if (!lua_isnil(L, -1)) {
            ref = lua_tointeger(L, -1);
            tag = MAR_TREF;
            buf_write(L, (void*)&tag, MAR_CHR, buf);
            buf_write(L, (void*)&ref, MAR_I32, buf);
            lua_pop(L, 1);
        }
        else {
            lua_pop(L, 1); /* pop nil */
            if (luaL_getmetafield(L, -1, "__persist")) {
                size_t length_position;
                tag = MAR_TUSR;

                lua_pushvalue(L, -2);
                lua_pushinteger(L, (*idx)++);
                lua_rawset(L, SEEN_IDX);

                lua_pushvalue(L, -2);
                lua_call(L, 1, 1);
                buf_write(L, (void*)&tag, MAR_CHR, buf);
                length_position = buf_begin_block(L, buf);
                if (lua_isfunction(L, -1)) {
                    lua_newtable(L);
                    lua_pushvalue(L, -2);
                    lua_rawseti(L, -2, 1);
                    lua_remove(L, -2);
                    mar_encode_table(L, buf, idx);
                }
                else if (lua_isstring(L, -1)) {
                    size_t source_len;
                    const char *source = lua_tolstring(L, -1, &source_len);
                    mar_encode_source_callback(L, buf, source, source_len, idx);
                }
                else {
                    luaL_error(L, "__persist must return a function or Lua source string");
                }
                buf_end_block(L, buf, length_position);
            }
            else {
                luaL_error(L, "attempt to encode userdata (no __persist hook)");
            }
            lua_pop(L, 1);
        }
        break;
    }
    case LUA_TNIL:
        buf_write(L, (void*)&val_type, MAR_CHR, buf);
        break;
    default:
        buf_write(L, (void*)&val_type, MAR_CHR, buf);
        luaL_error(L, "invalid value type (%s)", lua_typename(L, val_type));
    }
    if (pushed_value) lua_pop(L, 1);
}

static int mar_encode_table(lua_State *L, mar_Buffer *buf, size_t *idx)
{
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        mar_encode_value(L, buf, -2, idx);
        mar_encode_value(L, buf, -1, idx);
        lua_pop(L, 1);
    }
    return 1;
}

typedef enum mar_EncodeActionType {
    MAR_ENCODE_VALUE,
    MAR_ENCODE_TABLE,
    MAR_ENCODE_END_BLOCK
} mar_EncodeActionType;

typedef struct mar_EncodeAction {
    mar_EncodeActionType type;
    int value_ref;
    int key_ref;
    size_t length_position;
} mar_EncodeAction;

typedef struct mar_EncodeState {
    mar_Buffer buffer;
    mar_EncodeAction* actions;
    size_t action_count;
    size_t action_capacity;
    size_t idx;
    size_t read_offset;
    size_t encoded_size;
    int seen_ref;
    int iterator_keys_ref;
    int action_values_ref;
    int completed;
    int hint_updated;
    int action_failed;
} mar_EncodeState;

static void mar_unref(lua_State *L, int ref)
{
    if (ref >= 0) luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

static void mar_push_ref(lua_State *L, int ref)
{
    if (ref == LUA_REFNIL)
        lua_pushnil(L);
    else
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
}

static void mar_encode_action_reserve(lua_State *L, mar_EncodeState *state, size_t additional)
{
    size_t required;
    size_t capacity;
    mar_EncodeAction *actions;

    if (additional > SIZE_MAX - state->action_count) luaL_error(L, "encoder stack too deep");
    required = state->action_count + additional;
    if (required <= state->action_capacity) return;

    capacity = state->action_capacity ? state->action_capacity : 16;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity <<= 1;
    }
    if (capacity > SIZE_MAX / sizeof(*actions)) luaL_error(L, "encoder stack too deep");

    actions = realloc(state->actions, capacity * sizeof(*actions));
    if (!actions) luaL_error(L, "Out of memory!");
    state->actions = actions;
    state->action_capacity = capacity;
}

static mar_EncodeAction *mar_encode_action_push(lua_State *L, mar_EncodeState *state)
{
    mar_EncodeAction *action;
    mar_encode_action_reserve(L, state, 1);
    if (state->action_count >= INT_MAX) luaL_error(L, "encoder stack too deep");
    action = &state->actions[state->action_count++];
    action->type = MAR_ENCODE_VALUE;
    action->value_ref = (int)state->action_count;
    action->key_ref = LUA_NOREF;
    action->length_position = 0;
    return action;
}

static void mar_encode_action_release(lua_State *L, mar_EncodeAction *action)
{
    if (action->type == MAR_ENCODE_VALUE || action->type == MAR_ENCODE_TABLE) {
        lua_pushnil(L);
        lua_rawseti(L, ACTION_VALUES_IDX, action->value_ref);
    }
    action->value_ref = LUA_NOREF;
    action->key_ref = LUA_NOREF;
}

static void mar_encode_action_set(lua_State *L, int slot, int value_index)
{
    lua_pushvalue(L, value_index);
    lua_rawseti(L, ACTION_VALUES_IDX, slot);
}

static void mar_encode_action_push_value(lua_State *L, const mar_EncodeAction *action)
{
    lua_rawgeti(L, ACTION_VALUES_IDX, action->value_ref);
}

static void mar_encode_state_release(lua_State *L, mar_EncodeState *state)
{
    free(state->actions);
    state->actions = NULL;
    state->action_count = 0;
    state->action_capacity = 0;

    mar_unref(L, state->seen_ref);
    state->seen_ref = LUA_NOREF;
    mar_unref(L, state->iterator_keys_ref);
    state->iterator_keys_ref = LUA_NOREF;
    mar_unref(L, state->action_values_ref);
    state->action_values_ref = LUA_NOREF;

    free(state->buffer.data);
    state->buffer.data = NULL;
    state->buffer.size = 0;
    state->buffer.seek = 0;
    state->buffer.head = 0;
    state->encoded_size = 0;
}

static int mar_encode_state_gc(lua_State *L)
{
    mar_EncodeState *state = luaL_checkudata(L, 1, MAR_ENCODE_STATE_METATABLE);
    mar_encode_state_release(L, state);
    return 0;
}

static mar_EncodeState *mar_check_encode_state(lua_State *L, int index)
{
    return luaL_checkudata(L, index, MAR_ENCODE_STATE_METATABLE);
}

static void mar_encode_replace_with_table(
    lua_State *L,
    mar_EncodeState *state,
    size_t length_position)
{
    mar_EncodeAction *action;
    mar_EncodeAction *table_action;
    int table_ref;

    mar_encode_action_reserve(L, state, 1);
    action = &state->actions[state->action_count - 1];
    table_ref = action->value_ref;
    action->type = MAR_ENCODE_END_BLOCK;
    action->value_ref = LUA_NOREF;
    action->key_ref = LUA_NOREF;
    action->length_position = length_position;

    table_action = &state->actions[state->action_count++];
    table_action->type = MAR_ENCODE_TABLE;
    table_action->value_ref = table_ref;
    if (state->action_count > INT_MAX) luaL_error(L, "encoder stack too deep");
    table_action->key_ref = (int)state->action_count;
    table_action->length_position = 0;
}

static int mar_encode_incremental_scalar(lua_State *L, mar_EncodeState *state, int value_index)
{
    int value_type = lua_type(L, value_index);

    switch (value_type) {
    case LUA_TBOOLEAN: {
        int int_value = lua_toboolean(L, value_index);
        buf_reserve(L, MAR_CHR * 2, &state->buffer);
        state->buffer.data[state->buffer.head++] = (char)value_type;
        state->buffer.data[state->buffer.head++] = (char)int_value;
        return 1;
    }
    case LUA_TSTRING: {
        size_t length;
        const char *string_value = lua_tolstring(L, value_index, &length);
        uint32_t encoded_length;
        if (length > UINT32_MAX || length > SIZE_MAX - MAR_CHR - MAR_I32)
            luaL_error(L, "buffer too long");
        encoded_length = (uint32_t)length;
        buf_reserve(L, MAR_CHR + MAR_I32 + length, &state->buffer);
        state->buffer.data[state->buffer.head++] = (char)value_type;
        memcpy(&state->buffer.data[state->buffer.head], &encoded_length, MAR_I32);
        state->buffer.head += MAR_I32;
        memcpy(&state->buffer.data[state->buffer.head], string_value, length);
        state->buffer.head += length;
        return 1;
    }
    case LUA_TNUMBER: {
        lua_Number number_value = lua_tonumber(L, value_index);
        buf_reserve(L, MAR_CHR + MAR_I64, &state->buffer);
        state->buffer.data[state->buffer.head++] = (char)value_type;
        memcpy(&state->buffer.data[state->buffer.head], &number_value, MAR_I64);
        state->buffer.head += MAR_I64;
        return 1;
    }
    case LUA_TNIL:
        buf_write(L, (const char*)&value_type, MAR_CHR, &state->buffer);
        return 1;
    default:
        return 0;
    }
}

static void mar_encode_incremental_value(lua_State *L, mar_EncodeState *state)
{
    mar_EncodeAction *action;
    int value_index;
    int value_type;

    mar_encode_action_reserve(L, state, 1);
    action = &state->actions[state->action_count - 1];
    mar_encode_action_push_value(L, action);
    value_index = lua_gettop(L);
    value_type = lua_type(L, value_index);

    if (mar_encode_incremental_scalar(L, state, value_index)) {
        lua_pop(L, 1);
        mar_encode_action_release(L, action);
        state->action_count--;
        return;
    }

    switch (value_type) {
    case LUA_TTABLE: {
        int tag;
        int ref;
        size_t length_position;

        buf_write(L, (const char*)&value_type, MAR_CHR, &state->buffer);
        lua_pushvalue(L, value_index);
        lua_rawget(L, SEEN_IDX);
        if (!lua_isnil(L, -1)) {
            ref = (int)lua_tointeger(L, -1);
            tag = MAR_TREF;
            buf_write(L, (const char*)&tag, MAR_CHR, &state->buffer);
            buf_write(L, (const char*)&ref, MAR_I32, &state->buffer);
            lua_pop(L, 1);
            break;
        }
        lua_pop(L, 1);

        if (luaL_getmetafield(L, value_index, "__persist")) {
            tag = MAR_TUSR;
            lua_pushvalue(L, value_index);
            lua_call(L, 1, 1);
            if (!lua_isfunction(L, -1)) luaL_error(L, "__persist must return a function");

            lua_newtable(L);
            lua_pushvalue(L, -2);
            lua_rawseti(L, -2, 1);
            lua_remove(L, -2);
            mar_encode_action_set(L, action->value_ref, -1);

            buf_write(L, (const char*)&tag, MAR_CHR, &state->buffer);
            length_position = buf_begin_block(L, &state->buffer);
            lua_settop(L, value_index - 1);
            mar_encode_replace_with_table(L, state, length_position);
            return;
        }

        lua_pushvalue(L, value_index);
        lua_pushinteger(L, state->idx++);
        lua_rawset(L, SEEN_IDX);
        tag = MAR_TVAL;
        buf_write(L, (const char*)&tag, MAR_CHR, &state->buffer);
        length_position = buf_begin_block(L, &state->buffer);
        lua_pop(L, 1);
        mar_encode_replace_with_table(L, state, length_position);
        return;
    }
    case LUA_TFUNCTION: {
        int tag;
        int ref;
        int index;
        lua_Debug debug;
        size_t length_position;

        buf_write(L, (const char*)&value_type, MAR_CHR, &state->buffer);
        lua_pushvalue(L, value_index);
        lua_rawget(L, SEEN_IDX);
        if (!lua_isnil(L, -1)) {
            ref = (int)lua_tointeger(L, -1);
            tag = MAR_TREF;
            buf_write(L, (const char*)&tag, MAR_CHR, &state->buffer);
            buf_write(L, (const char*)&ref, MAR_I32, &state->buffer);
            lua_pop(L, 1);
            break;
        }
        lua_pop(L, 1);

        lua_pushvalue(L, value_index);
        lua_getinfo(L, ">nuS", &debug);
        if (debug.what[0] != 'L') luaL_error(L, "attempt to persist a C function '%s'", debug.name);

        tag = MAR_TVAL;
        lua_pushvalue(L, value_index);
        lua_pushinteger(L, state->idx++);
        lua_rawset(L, SEEN_IDX);

        buf_write(L, (const char*)&tag, MAR_CHR, &state->buffer);
        length_position = buf_begin_block(L, &state->buffer);
        lua_pushvalue(L, value_index);
        lua_dump(L, (lua_Writer)buf_write, &state->buffer);
        buf_end_block(L, &state->buffer, length_position);
        lua_pop(L, 1);

        lua_newtable(L);
        for (index = 1; index <= debug.nups; index++) {
            lua_getupvalue(L, value_index, index);
            lua_rawseti(L, -2, index);
        }
        mar_encode_action_set(L, action->value_ref, -1);
        length_position = buf_begin_block(L, &state->buffer);
        lua_settop(L, value_index - 1);
        mar_encode_replace_with_table(L, state, length_position);
        return;
    }
    case LUA_TUSERDATA: {
        int tag;
        int ref;
        size_t length_position;

        buf_write(L, (const char*)&value_type, MAR_CHR, &state->buffer);
        lua_pushvalue(L, value_index);
        lua_rawget(L, SEEN_IDX);
        if (!lua_isnil(L, -1)) {
            ref = (int)lua_tointeger(L, -1);
            tag = MAR_TREF;
            buf_write(L, (const char*)&tag, MAR_CHR, &state->buffer);
            buf_write(L, (const char*)&ref, MAR_I32, &state->buffer);
            lua_pop(L, 1);
            break;
        }
        lua_pop(L, 1);

        if (!luaL_getmetafield(L, value_index, "__persist"))
            luaL_error(L, "attempt to encode userdata (no __persist hook)");

        lua_pushvalue(L, value_index);
        lua_pushinteger(L, state->idx++);
        lua_rawset(L, SEEN_IDX);
        lua_pushvalue(L, value_index);
        lua_call(L, 1, 1);

        tag = MAR_TUSR;
        buf_write(L, (const char*)&tag, MAR_CHR, &state->buffer);
        length_position = buf_begin_block(L, &state->buffer);
        if (lua_isfunction(L, -1)) {
            lua_newtable(L);
            lua_pushvalue(L, -2);
            lua_rawseti(L, -2, 1);
            lua_remove(L, -2);
            mar_encode_action_set(L, action->value_ref, -1);
            lua_settop(L, value_index - 1);
            mar_encode_replace_with_table(L, state, length_position);
            return;
        }
        if (lua_isstring(L, -1)) {
            size_t source_length;
            const char *source = lua_tolstring(L, -1, &source_length);
            mar_encode_source_callback(L, &state->buffer, source, source_length, &state->idx);
            buf_end_block(L, &state->buffer, length_position);
            lua_pop(L, 1);
            break;
        }
        luaL_error(L, "__persist must return a function or Lua source string");
        break;
    }
    default:
        buf_write(L, (const char*)&value_type, MAR_CHR, &state->buffer);
        luaL_error(L, "invalid value type (%s)", lua_typename(L, value_type));
    }

    lua_pop(L, 1);
    mar_encode_action_release(L, action);
    state->action_count--;
}

static void mar_encode_incremental_table(lua_State *L, mar_EncodeState *state)
{
    mar_EncodeAction *action;
    int table_index;

    mar_encode_action_reserve(L, state, 2);
    action = &state->actions[state->action_count - 1];
    mar_encode_action_push_value(L, action);
    table_index = lua_gettop(L);
    lua_rawgeti(L, ITERATOR_KEYS_IDX, action->key_ref);
    if (!lua_next(L, table_index)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_rawseti(L, ITERATOR_KEYS_IDX, action->key_ref);
        mar_encode_action_release(L, action);
        state->action_count--;
        return;
    }

    lua_pushvalue(L, -2);
    lua_rawseti(L, ITERATOR_KEYS_IDX, action->key_ref);

    if (mar_encode_incremental_scalar(L, state, -2)) {
        if (mar_encode_incremental_scalar(L, state, -1)) {
            lua_settop(L, table_index - 1);
            return;
        }
        else {
            mar_EncodeAction *value_action;

            value_action = mar_encode_action_push(L, state);
            value_action->type = MAR_ENCODE_VALUE;
            mar_encode_action_set(L, value_action->value_ref, -1);
            lua_settop(L, table_index - 1);
            return;
        }
    }

    {
        mar_EncodeAction *value_action;
        mar_EncodeAction *key_action;

        value_action = mar_encode_action_push(L, state);
        value_action->type = MAR_ENCODE_VALUE;
        mar_encode_action_set(L, value_action->value_ref, -1);

        key_action = mar_encode_action_push(L, state);
        key_action->type = MAR_ENCODE_VALUE;
        mar_encode_action_set(L, key_action->value_ref, -2);
        lua_settop(L, table_index - 1);
    }
}

static void mar_encode_incremental_action(lua_State *L, mar_EncodeState *state)
{
    mar_EncodeAction *action = &state->actions[state->action_count - 1];
    if (action->type == MAR_ENCODE_VALUE) {
        mar_encode_incremental_value(L, state);
    }
    else if (action->type == MAR_ENCODE_TABLE) {
        mar_encode_incremental_table(L, state);
    }
    else {
        buf_end_block(L, &state->buffer, action->length_position);
        state->action_count--;
    }
}

static int mar_encode_begin(lua_State *L)
{
    const unsigned char magic = MAR_MAGIC;
    size_t constants_length;
    size_t index;
    int seen_index;
    mar_EncodeState *state;
    mar_EncodeAction *root_action;

    if (lua_isnone(L, 1)) lua_pushnil(L);
    if (lua_isnoneornil(L, 2)) {
        lua_newtable(L);
    }
    else if (!lua_istable(L, 2)) {
        luaL_error(L, "bad argument #2 to encode_begin (expected table)");
    }
    lua_settop(L, 2);

    state = lua_newuserdata(L, sizeof(*state));
    memset(state, 0, sizeof(*state));
    state->seen_ref = LUA_NOREF;
    state->iterator_keys_ref = LUA_NOREF;
    state->action_values_ref = LUA_NOREF;
    luaL_getmetatable(L, MAR_ENCODE_STATE_METATABLE);
    lua_setmetatable(L, -2);

    constants_length = lua_objlen(L, 2);
    lua_newtable(L);
    seen_index = lua_gettop(L);
    for (index = 1; index <= constants_length; index++) {
        lua_rawgeti(L, 2, (int)index);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushinteger(L, index);
        lua_rawset(L, seen_index);
    }
    state->idx = index;
    state->seen_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    state->iterator_keys_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    state->action_values_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    buf_init(L, &state->buffer, mar_encode_buffer_size_hint);
    buf_write(L, (const char*)&magic, MAR_CHR, &state->buffer);
    root_action = mar_encode_action_push(L, state);
    mar_push_ref(L, state->action_values_ref);
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, root_action->value_ref);
    lua_pop(L, 1);
    return 1;
}

static int mar_encode_step(lua_State *L)
{
    lua_Integer operation_budget;
    lua_Integer operation;
    mar_EncodeState *state = mar_check_encode_state(L, 1);

    operation_budget = luaL_optinteger(L, 2, 256);
    if (operation_budget < 1) luaL_error(L, "bad argument #2 to encode_step (expected positive integer)");
    if (!state->buffer.data) luaL_error(L, "encoder state is closed");
    if (state->action_failed) luaL_error(L, "encoder state failed during a previous step");

    lua_settop(L, 2);
    mar_push_ref(L, state->seen_ref);
    mar_push_ref(L, state->iterator_keys_ref);
    mar_push_ref(L, state->action_values_ref);
    for (operation = 0; operation < operation_budget && state->action_count; operation++) {
        state->action_failed = 1;
        mar_encode_incremental_action(L, state);
        state->action_failed = 0;
    }

    if (!state->action_count) {
        state->completed = 1;
        state->encoded_size = state->buffer.head;
        if (!state->hint_updated) {
            const size_t maximum_hint = 8 * 1024 * 1024;
            const size_t encoded_size =
                state->buffer.head < maximum_hint ? state->buffer.head : maximum_hint;
            size_t next_hint = encoded_size + encoded_size / 8 + 128;
            if (next_hint > maximum_hint) next_hint = maximum_hint;
            if (next_hint < 128) next_hint = 128;
            mar_encode_buffer_size_hint = next_hint;
            state->hint_updated = 1;
        }
    }

    lua_pushboolean(L, state->completed);
    return 1;
}

static int mar_encode_read(lua_State *L)
{
    lua_Integer byte_budget;
    size_t remaining;
    size_t length;
    mar_EncodeState *state = mar_check_encode_state(L, 1);

    byte_budget = luaL_optinteger(L, 2, 64 * 1024);
    if (byte_budget < 1) luaL_error(L, "bad argument #2 to encode_read (expected positive integer)");
    if (!state->completed) luaL_error(L, "encoder state is not complete");
    if (!state->buffer.data) luaL_error(L, "encoder state is closed");

    remaining = state->encoded_size - state->read_offset;
    length = remaining < (size_t)byte_budget ? remaining : (size_t)byte_budget;
    state->read_offset += length;
    lua_pushboolean(L, state->read_offset == state->encoded_size);
    lua_pushlstring(L, state->buffer.data + state->read_offset - length, length);
    return 2;
}

static int mar_encode_size(lua_State *L)
{
    mar_EncodeState *state = mar_check_encode_state(L, 1);
    if (!state->completed) luaL_error(L, "encoder state is not complete");
    lua_pushnumber(L, (lua_Number)state->encoded_size);
    return 1;
}

static void mar_require_bytes(
    lua_State *L, const char *buf, size_t len, const char *position, size_t count)
{
    size_t offset = (size_t)(position - buf);
    if (offset > len || count > len - offset) luaL_error(L, "bad code");
}

#define mar_incr_ptr(l) do { \
    size_t mar_length = (size_t)(l); \
    mar_require_bytes(L, buf, len, *p, mar_length); \
    (*p) += mar_length; \
} while (0)

#define mar_next_len(l,T) do { \
    T mar_value; \
    mar_require_bytes(L, buf, len, *p, sizeof(T)); \
    memcpy(&mar_value, *p, sizeof(T)); \
    (l) = mar_value; \
    (*p) += sizeof(T); \
} while (0)

static void mar_decode_value
    (lua_State *L, const char *buf, size_t len, const char **p, size_t *idx)
{
    size_t l;
    char val_type;
    mar_require_bytes(L, buf, len, *p, MAR_CHR);
    val_type = **p;
    mar_incr_ptr(MAR_CHR);
    switch (val_type) {
    case LUA_TBOOLEAN:
        mar_require_bytes(L, buf, len, *p, MAR_CHR);
        lua_pushboolean(L, *(const char*)*p);
        mar_incr_ptr(MAR_CHR);
        break;
    case LUA_TNUMBER: {
        lua_Number value;
        mar_require_bytes(L, buf, len, *p, MAR_I64);
        memcpy(&value, *p, MAR_I64);
        lua_pushnumber(L, value);
        mar_incr_ptr(MAR_I64);
        break;
    }
    case LUA_TSTRING:
        mar_next_len(l, uint32_t);
        mar_require_bytes(L, buf, len, *p, l);
        lua_pushlstring(L, *p, l);
        mar_incr_ptr(l);
        break;
    case LUA_TTABLE: {
        char tag;
        mar_require_bytes(L, buf, len, *p, MAR_CHR);
        tag = *(const char*)*p;
        mar_incr_ptr(MAR_CHR);
        if (tag == MAR_TREF) {
            int ref;
            mar_next_len(ref, int);
            lua_rawgeti(L, SEEN_IDX, ref);
        }
        else if (tag == MAR_TVAL) {
            mar_next_len(l, uint32_t);
            mar_require_bytes(L, buf, len, *p, l);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_rawseti(L, SEEN_IDX, (*idx)++);
            mar_decode_table(L, *p, l, idx);
            mar_incr_ptr(l);
        }
        else if (tag == MAR_TUSR) {
            mar_next_len(l, uint32_t);
            mar_require_bytes(L, buf, len, *p, l);
            lua_newtable(L);
            mar_decode_table(L, *p, l, idx);
            lua_rawgeti(L, -1, 1);
            lua_call(L, 0, 1);
            lua_remove(L, -2);
            mar_incr_ptr(l);
        }
        else {
            luaL_error(L, "bad encoded data");
        }
        break;
    }
    case LUA_TFUNCTION: {
        size_t nups;
        int i;
        mar_Buffer dec_buf;
        char tag;
        mar_require_bytes(L, buf, len, *p, MAR_CHR);
        tag = *(const char*)*p;
        mar_incr_ptr(MAR_CHR);
        if (tag == MAR_TREF) {
            int ref;
            mar_next_len(ref, int);
            lua_rawgeti(L, SEEN_IDX, ref);
        }
        else {
            mar_next_len(l, uint32_t);
            mar_require_bytes(L, buf, len, *p, l);
            dec_buf.data = (char*)*p;
            dec_buf.size = l;
            dec_buf.head = l;
            dec_buf.seek = 0;
            if (lua_load(L, (lua_Reader)buf_read, &dec_buf, "=marshal") != 0)
                luaL_error(L, "cannot decode persisted Lua function: %s", lua_tostring(L, -1));
            mar_incr_ptr(l);

            lua_pushvalue(L, -1);
            lua_rawseti(L, SEEN_IDX, (*idx)++);

            mar_next_len(l, uint32_t);
            mar_require_bytes(L, buf, len, *p, l);
            lua_newtable(L);
            mar_decode_table(L, *p, l, idx);
            nups = lua_objlen(L, -1);
            for (i=1; i <= nups; i++) {
                lua_rawgeti(L, -1, i);
                lua_setupvalue(L, -3, i);
            }
            lua_pop(L, 1);
            mar_incr_ptr(l);
        }
        break;
    }
    case LUA_TUSERDATA: {
        char tag;
        mar_require_bytes(L, buf, len, *p, MAR_CHR);
        tag = *(const char*)*p;
        mar_incr_ptr(MAR_CHR);
        if (tag == MAR_TREF) {
            int ref;
            mar_next_len(ref, int);
            lua_rawgeti(L, SEEN_IDX, ref);
        }
        else if (tag == MAR_TUSR) {
            int userdata_ref;
            mar_next_len(l, uint32_t);
            mar_require_bytes(L, buf, len, *p, l);
            /* Encoder assigns the userdata reference before its callback. */
            userdata_ref = (int)(*idx)++;
            lua_newtable(L);
            mar_decode_table(L, *p, l, idx);
            lua_rawgeti(L, -1, 1);
            lua_call(L, 0, 1);
            lua_remove(L, -2);
            lua_pushvalue(L, -1);
            lua_rawseti(L, SEEN_IDX, userdata_ref);
            mar_incr_ptr(l);
        }
        else { /* tag == MAR_TVAL */
            lua_pushnil(L);
        }
        break;
    }
    case LUA_TNIL:
    case LUA_TTHREAD:
        lua_pushnil(L);
        break;
    default:
        luaL_error(L, "bad code");
    }
}

static int mar_decode_table(lua_State *L, const char* buf, size_t len, size_t *idx)
{
    const char* p;
    p = buf;
    while (p - buf < len) {
        mar_decode_value(L, buf, len, &p, idx);
        mar_decode_value(L, buf, len, &p, idx);
        lua_settable(L, -3);
    }
    return 1;
}

static int mar_encode(lua_State* L)
{
    const unsigned char m = MAR_MAGIC;
    size_t idx, len;
    mar_Buffer buf;

    if (lua_isnone(L, 1)) {
        lua_pushnil(L);
    }
    if (lua_isnoneornil(L, 2)) {
        lua_newtable(L);
    }
    else if (!lua_istable(L, 2)) {
        luaL_error(L, "bad argument #2 to encode (expected table)");
    }
    lua_settop(L, 2);

    len = lua_objlen(L, 2);
    lua_newtable(L);
    for (idx = 1; idx <= len; idx++) {
        lua_rawgeti(L, 2, idx);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushinteger(L, idx);
        lua_rawset(L, SEEN_IDX);
    }
    lua_pushvalue(L, 1);

    buf_init(L, &buf, mar_encode_buffer_size_hint);
    buf_write(L, (void*)&m, 1, &buf);

    mar_encode_value(L, &buf, -1, &idx);

    lua_pop(L, 1);

    lua_pushlstring(L, buf.data, buf.head);

    {
        const size_t maximum_hint = 8 * 1024 * 1024;
        const size_t encoded_size = buf.head < maximum_hint ? buf.head : maximum_hint;
        size_t next_hint = encoded_size + encoded_size / 8 + 128;
        if (next_hint > maximum_hint) next_hint = maximum_hint;
        if (next_hint < 128) next_hint = 128;
        mar_encode_buffer_size_hint = next_hint;
    }

    buf_done(L, &buf);

    lua_remove(L, SEEN_IDX);

    return 1;
}

static int mar_decode(lua_State* L)
{
    size_t l, idx, len;
    const char *p;
    const char *s = luaL_checklstring(L, 1, &l);

    if (l < 1) luaL_error(L, "bad header");
    if (*(unsigned char *)s++ != MAR_MAGIC) luaL_error(L, "bad magic");
    l -= 1;

    if (lua_isnoneornil(L, 2)) {
        lua_newtable(L);
    }
    else if (!lua_istable(L, 2)) {
        luaL_error(L, "bad argument #2 to decode (expected table)");
    }
    lua_settop(L, 2);

    len = lua_objlen(L, 2);
    lua_newtable(L);
    for (idx = 1; idx <= len; idx++) {
        lua_rawgeti(L, 2, idx);
        lua_rawseti(L, SEEN_IDX, idx);
    }

    p = s;
    mar_decode_value(L, s, l, &p, &idx);

    lua_remove(L, SEEN_IDX);
    lua_remove(L, 2);

    return 1;
}

static int mar_clone(lua_State* L)
{
    mar_encode(L);
    lua_replace(L, 1);
    mar_decode(L);
    return 1;
}

static const luaL_Reg marshallib[] =
{
    {"encode",      mar_encode},
    {"encode_begin", mar_encode_begin},
    {"encode_step", mar_encode_step},
    {"encode_read", mar_encode_read},
    {"encode_size", mar_encode_size},
    {"decode",      mar_decode},
    {"clone",       mar_clone},
    {NULL,	    NULL}
};

int luaopen_marshal(lua_State *L)
{
    if (luaL_newmetatable(L, MAR_ENCODE_STATE_METATABLE)) {
        lua_pushcfunction(L, mar_encode_state_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushliteral(L, "marshal encode state");
        lua_setfield(L, -2, "__metatable");
    }
    lua_pop(L, 1);

    /*lua_newtable(L);
    luaL_register(L, NULL, R);*/
    luaL_register(L, "marshal", marshallib);
    return 1;
}

