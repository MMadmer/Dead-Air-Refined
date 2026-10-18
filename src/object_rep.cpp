// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.

#define LUABIND_BUILDING

#include <luabind/detail/object_rep.hpp>
#include <luabind/detail/class_rep.hpp>

#if LUA_VERSION_NUM < 502
# define lua_getuservalue lua_getfenv
# define lua_setuservalue lua_setfenv
#endif

#if __has_include(<luajit.h>)
extern "C" {
# include <luajit.h>
}
#endif

namespace luabind {
	namespace detail {

		// dest is a function that is called to delete the c++ object this struct holds
		object_rep::object_rep(instance_holder* instance, class_rep* crep)
			: m_instance(instance)
			, m_classrep(crep)
		{
		}

		object_rep::~object_rep()
		{
			if(!m_instance)
				return;
			m_instance->~instance_holder();
			deallocate(m_instance);
		}

		namespace
		{
			// The keys of the dependencies in an instance's own uservalue table: light userdata no script can make,
			// so neither a member a script stores there nor a lookup meets them.
			char dependency_key;
			char dependency_set_key;
		}

		// The objects an instance keeps alive go into its own uservalue table (where a member a script stores on it
		// lands): the collector follows them from the instance, so they take no registry reference and the instance
		// no finalizer to release one. The first is stored itself, any more in a set.
		void object_rep::add_dependency(lua_State* L, int self, int index)
		{
			if(self < 0 && self > LUA_REGISTRYINDEX)
				self = lua_gettop(L) + self + 1;
			if(index < 0 && index > LUA_REGISTRYINDEX)
				index = lua_gettop(L) + index + 1;
			if(lua_isnoneornil(L, index))
				return;

			// The class table until the instance has a table of its own, which then has the class table as metatable.
			lua_getuservalue(L, self);
			if(lua_getmetatable(L, -1))
			{
				lua_pop(L, 1);
			}
			else
			{
				lua_newtable(L);
				lua_insert(L, -2);
				lua_setmetatable(L, -2);
				lua_pushvalue(L, -1);
				lua_setuservalue(L, self);
			}

			lua_pushlightuserdata(L, &dependency_key);
			lua_rawget(L, -2);
			if(lua_isnil(L, -1))
			{
				lua_pop(L, 1);
				lua_pushlightuserdata(L, &dependency_key);
				lua_pushvalue(L, index);
				lua_rawset(L, -3);
			}
			else if(lua_rawequal(L, -1, index))
			{
				lua_pop(L, 1);
			}
			else
			{
				lua_pop(L, 1);
				lua_pushlightuserdata(L, &dependency_set_key);
				lua_rawget(L, -2);
				if(lua_isnil(L, -1))
				{
					lua_pop(L, 1);
					lua_newtable(L);
					lua_pushlightuserdata(L, &dependency_set_key);
					lua_pushvalue(L, -2);
					lua_rawset(L, -4);
				}
				lua_pushvalue(L, index);
				lua_pushboolean(L, 1);
				lua_rawset(L, -3);
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		}

		namespace
		{
			int get_instance_value(lua_State* L);
		}

		int destroy_instance(lua_State* L)
		{
			object_rep* instance = static_cast<object_rep*>(lua_touserdata(L, 1));

			// instance.__finalize. While the instance's __index is still luabind's own, its lookup is run directly
			// (the uservalue, its metatable, a property's getter): the metamethod's way through the VM cost every
			// collected instance a C call. A metatable a script has changed takes the full lookup.
			lua_pushliteral(L, "__finalize");
			bool direct = false;
			if(lua_getmetatable(L, 1))
			{
				lua_pushliteral(L, "__index");
				lua_rawget(L, -2);
				direct = lua_tocfunction(L, -1) == &get_instance_value;
				lua_pop(L, 2);
			}
			// The stack is (instance, key), as __index gets it; the value ends on top either way.
			if(direct)
				get_instance_value(L);
			else
				lua_gettable(L, 1);

			if(lua_isnil(L, -1))
			{
				lua_pop(L, 1);
			}
			else
			{
				lua_pushvalue(L, 1);
				lua_call(L, 1, 0);
			}

			instance->~object_rep();

			lua_pushnil(L);
			lua_setmetatable(L, 1);
			return 0;
		}

		namespace
		{

			int set_instance_value(lua_State* L)
			{
				lua_getuservalue(L, 1);
				lua_pushvalue(L, 2);
				lua_rawget(L, -2);

				if(lua_isnil(L, -1) && lua_getmetatable(L, -2))
				{
					lua_pushvalue(L, 2);
					lua_rawget(L, -2);
					lua_replace(L, -3);
					lua_pop(L, 1);
				}

				if(lua_tocfunction(L, -1) == &property_tag)
				{
					// this member is a property, extract the "set" function and call it.
					lua_getupvalue(L, -1, 2);

					if(lua_isnil(L, -1))
					{
						lua_pushfstring(L, "property '%s' is read only", lua_tostring(L, 2));
						lua_error(L);
					}

					lua_pushvalue(L, 1);
					lua_pushvalue(L, 3);
					lua_call(L, 2, 0);
					return 0;
				}

				lua_pop(L, 1);

				if(!lua_getmetatable(L, 4))
				{
					lua_newtable(L);
					lua_pushvalue(L, -1);
					lua_setuservalue(L, 1);
					lua_pushvalue(L, 4);
					lua_setmetatable(L, -2);
				}
				else
				{
					lua_pop(L, 1);
				}

				lua_pushvalue(L, 2);
				lua_pushvalue(L, 3);
				lua_rawset(L, -3);

				return 0;
			}

			int get_instance_value(lua_State* L)
			{
				lua_getuservalue(L, 1);
				lua_pushvalue(L, 2);
				lua_rawget(L, -2);

				if(lua_isnil(L, -1) && lua_getmetatable(L, -2))
				{
					lua_pushvalue(L, 2);
					lua_rawget(L, -2);
				}

				if(lua_tocfunction(L, -1) == &property_tag)
				{
					// this member is a property, extract the "get" function and call it.
					lua_getupvalue(L, -1, 1);
					lua_pushvalue(L, 1);
					lua_call(L, 1, 1);
				}

				return 1;
			}

			int dispatch_operator(lua_State* L)
			{
				for(int i = 0; i < 2; ++i)
				{
					if(get_instance(L, 1 + i))
					{
						int nargs = lua_gettop(L);

						lua_pushvalue(L, lua_upvalueindex(1));
						lua_gettable(L, 1 + i);

						if(lua_isnil(L, -1))
						{
							lua_pop(L, 1);
							continue;
						}

						lua_insert(L, 1); // move the function to the bottom

						nargs = lua_toboolean(L, lua_upvalueindex(2)) ? 1 : nargs;

						if(lua_toboolean(L, lua_upvalueindex(2))) // remove trailing nil
							lua_remove(L, 3);

						lua_call(L, nargs, 1);
						return 1;
					}
				}

				lua_pop(L, lua_gettop(L));
				lua_pushstring(L, "No such operator defined");
				lua_error(L);

				return 0;
			}

		} // namespace unnamed

		LUABIND_API void push_instance_metatable(lua_State* L)
		{
			lua_newtable(L);

			// This is used as a tag to determine if a userdata is a luabind
			// instance. We use a numeric key and a cclosure for fast comparison.
			lua_pushnumber(L, 1);
			lua_pushcclosure(L, get_instance_value, 0);
			lua_rawset(L, -3);

			lua_pushcclosure(L, destroy_instance, 0);
			lua_setfield(L, -2, "__gc");

			lua_pushcclosure(L, get_instance_value, 0);
			lua_setfield(L, -2, "__index");

			lua_pushcclosure(L, set_instance_value, 0);
			lua_setfield(L, -2, "__newindex");

			for(int op = 0; op < number_of_operators; ++op)
			{
				lua_pushstring(L, get_operator_name(op));
				lua_pushvalue(L, -1);
				lua_pushboolean(L, op == op_unm || op == op_len);
				lua_pushcclosure(L, &dispatch_operator, 2);
				lua_settable(L, -3);
			}
		}

		LUABIND_API object_rep* get_instance(lua_State* L, int index)
		{
			object_rep* result = static_cast<object_rep*>(lua_touserdata(L, index));

			if(!result || !lua_getmetatable(L, index))
				return 0;

			lua_rawgeti(L, -1, 1);

			if(lua_tocfunction(L, -1) != &get_instance_value)
				result = 0;

			lua_pop(L, 2);

			return result;
		}

		LUABIND_API void instance_needs_no_finalizer(lua_State* L, int index)
		{
#ifdef LUAJIT_VERSION
			luaJIT_udata_nofinalize(L, index);
#else
			(void)L;
			(void)index;
#endif
		}

		LUABIND_API object_rep* push_new_instance(lua_State* L, class_rep* cls)
		{
			void* storage = lua_newuserdata(L, sizeof(object_rep));
			object_rep* result = new (storage) object_rep(0, cls);
			cls->get_table(L);
			lua_setuservalue(L, -2);
			lua_rawgeti(L, LUA_REGISTRYINDEX, cls->metatable_ref());
			lua_setmetatable(L, -2);
			return result;
		}

	}
}

