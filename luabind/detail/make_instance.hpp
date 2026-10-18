// Copyright Daniel Wallin 2009. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef LUABIND_DETAIL_MAKE_INSTANCE_090310_HPP
# define LUABIND_DETAIL_MAKE_INSTANCE_090310_HPP

# include <luabind/detail/inheritance.hpp>
# include <luabind/detail/object_rep.hpp>

namespace luabind {
	namespace detail {

		// The registry's class maps under keys of their own addresses: the lookups of every instance a function
		// returns hash no string. The named keys ("__luabind_class_map", "__luabind_class_id_map") stay.
		LUABIND_API class_map& registry_class_map(lua_State* L);
		LUABIND_API class_id_map& registry_class_id_map(lua_State* L);

		template <class T>
		std::pair<class_id, void*> get_dynamic_class_aux(lua_State* L, T const* p, std::true_type)
		{
			class_id_map& class_ids = registry_class_id_map(L);

			return std::make_pair(class_ids.get_local(typeid(*p)), dynamic_cast<void*>(const_cast<T*>(p)));
		}

		template <class T>
		std::pair<class_id, void*> get_dynamic_class_aux(lua_State*, T const* p, std::false_type)
		{
			return std::make_pair(registered_class<T>::id, (void*)p);
		}

		template <class T>
		std::pair<class_id, void*> get_dynamic_class(lua_State* L, T* p)
		{
			return get_dynamic_class_aux(L, p, std::is_polymorphic<T>());
		}

		template <class T>
		class_rep* get_pointee_class(class_map const& classes, T*)
		{
			return classes.get(registered_class<T>::id);
		}

		template <class P>
		class_rep* get_pointee_class(lua_State* L, P const& p, class_id dynamic_id)
		{
			class_map const& classes = registry_class_map(L);

			class_rep* cls = classes.get(dynamic_id);

			if(!cls) {
				cls = get_pointee_class(classes, get_pointer(p));
			}

			return cls;
		}

		// Holders whose destruction does nothing: a raw pointer the instance does not own, a value with a trivial
		// destructor. Kept inline in an instance of a C++ class, they leave it nothing to finalize.
		template <class Holder>
		struct holder_destruction_trivial : std::false_type {};

		template <class T, class Pointee>
		struct holder_destruction_trivial<pointer_holder<T*, Pointee>> : std::true_type {};

		template <class T>
		struct holder_destruction_trivial<value_holder<T>> : std::is_trivially_destructible<T> {};

		template <class Holder>
		void release_trivial_instance(lua_State* L, object_rep* instance, void const* storage)
		{
			if constexpr(holder_destruction_trivial<Holder>::value)
			{
				if(instance->holds_inline(storage) && instance->crep()->get_class_type() == class_rep::cpp_class)
					instance_needs_no_finalizer(L, -1);
			}
		}

		// Create an appropriate instance holder for the given pointer like object.
		template <class P>
		void make_pointer_instance(lua_State* L, P p)
		{
			std::pair<class_id, void*> dynamic = get_dynamic_class(L, get_pointer(p));

			class_rep* cls = get_pointee_class(L, p, dynamic.first);

			if(!cls)
			{
				throw unresolved_name("Trying to use unregistered class", typeid(P).name());
			}

			object_rep* instance = push_new_instance(L, cls);

			using value_type = typename std::remove_reference<P>::type;
			using holder_type = pointer_holder<value_type>;

			void* storage = instance->allocate(sizeof(holder_type));

			try
			{
				new (storage) holder_type(std::move(p), dynamic.first, dynamic.second);
			}
			catch(...)
			{
				instance->deallocate(storage);
				lua_pop(L, 1);
				throw;
			}

			instance->set_instance(static_cast<holder_type*>(storage));
			release_trivial_instance<holder_type>(L, instance, storage);
		}


		template< typename ValueType >
		void make_value_instance(lua_State* L, ValueType&& val, std::true_type /* is smart ptr */)
		{
			if(get_pointer(val)) {
				std::pair<class_id, void*> dynamic = get_dynamic_class(L, get_pointer(val));
				class_rep* cls = get_pointee_class(L, val, dynamic.first);

				using pointee_type = decltype(*get_pointer(val));

				if(!cls) {
					throw unresolved_name("Trying to use unregistered class", typeid(pointee_type).name());
				}

				object_rep* instance = push_new_instance(L, cls);

				using value_type = typename std::remove_reference<ValueType>::type;
				using holder_type = pointer_like_holder<value_type>;

				void* storage = instance->allocate(sizeof(holder_type));

				try {
					new (storage) holder_type(L, std::forward<ValueType>(val), dynamic.first, dynamic.second);
				}
				catch(...) {
					instance->deallocate(storage);
					lua_pop(L, 1);
					throw;
				}

				instance->set_instance(static_cast<holder_type*>(storage));
			} else {
				lua_pushnil(L);
			}
		}

		template< typename ValueType >
		void make_value_instance(lua_State* L, ValueType&& val, std::false_type /* smart ptr */)
		{
			const auto value_type_id = detail::registered_class<ValueType>::id;
			class_rep* cls = get_pointee_class(L, &val, value_type_id);

			if(!cls) {
				throw unresolved_name("Trying to use unregistered class: ", typeid(ValueType).name());
			}

			object_rep* instance = push_new_instance(L, cls);

			using value_type = typename std::remove_reference<ValueType>::type;
			using holder_type = value_holder<value_type>;

			void* storage = instance->allocate(sizeof(holder_type));

			try {
				new (storage) holder_type(L, std::forward<ValueType>(val));
			}
			catch(...) {
				instance->deallocate(storage);
				lua_pop(L, 1);
				throw;
			}

			instance->set_instance(static_cast<holder_type*>(storage));
			release_trivial_instance<holder_type>(L, instance, storage);
		}

		template< typename ValueType >
		void make_value_instance(lua_State* L, ValueType&& val)
		{
			make_value_instance(L, std::forward<ValueType>(val), has_get_pointer<ValueType>());
		}

	} // namespace detail
} // namespace luabind

#endif // LUABIND_DETAIL_MAKE_INSTANCE_090310_HPP

