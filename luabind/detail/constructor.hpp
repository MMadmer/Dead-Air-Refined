// Copyright Daniel Wallin 2008. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef LUABIND_DETAIL_CONSTRUCTOR_081018_HPP
#define LUABIND_DETAIL_CONSTRUCTOR_081018_HPP

#include <luabind/get_main_thread.hpp>
#include <luabind/lua_argument_proxy.hpp>
#include <luabind/wrapper_base.hpp>
#include <luabind/detail/inheritance.hpp>
#include <luabind/detail/instance_holder.hpp>
#include <luabind/detail/object_rep.hpp>
#include <type_traits>

namespace luabind {
	namespace detail {

		inline void inject_backref(lua_State*, void*, void*)
		{}

		template <class T>
		void inject_backref(lua_State* L, T* p, wrap_base*)
		{
			weak_ref(get_main_thread(L), L, 1).swap(wrap_access::ref(*p));
		}

		// A value the class owns the default way, trivial to destroy and small enough, is built inside the instance:
		// no heap blocks for it and its holder, and nothing to finalize. The pointer holder's release() gave an adopted
		// object to C++, so the classes C++ adopts (polymorphic ones, all with destructors) keep it.
		template< class T, class Pointer >
		struct construct_in_place : std::integral_constant< bool,
			std::is_same< Pointer, luabind::unique_ptr< T > >::value
			&& std::is_trivially_destructible< T >::value
			&& std::is_move_constructible< T >::value
			&& !std::is_polymorphic< T >::value
			&& !std::is_base_of< wrap_base, T >::value
			&& sizeof(value_holder< T >) <= object_rep::inline_capacity >
		{};

		template< class T, class Pointer, class Signature, class Arguments, class ArgumentIndices >
		struct construct_aux_helper;

		template< class T, class Pointer, class Signature, typename... Arguments, unsigned int... ArgumentIndices >
		struct construct_aux_helper< T, Pointer, Signature, meta::type_list< Arguments... >, meta::index_list< ArgumentIndices... > >
		{
			using holder_type = pointer_holder<Pointer, T>;

			void operator()(argument const& self_, Arguments... args) const
			{
				object_rep* self = touserdata<object_rep>(self_);

				if constexpr(construct_in_place<T, Pointer>::value)
				{
					using value_holder_type = value_holder<T>;
					void* storage = self->allocate(sizeof(value_holder_type));
					self->set_instance(new (storage) value_holder_type(self_.interpreter(), T(args...)));
					if(self->crep()->get_class_type() == class_rep::cpp_class)
						instance_needs_no_finalizer(self_.interpreter(), self_.index());
					return;
				}

                luabind::unique_ptr<T> instance(luabind_new<T>(args...));
				inject_backref(self_.interpreter(), instance.get(), instance.get());

				void* naked_ptr = instance.get();
				Pointer ptr(instance.release());

				void* storage = self->allocate(sizeof(holder_type));

				self->set_instance(new (storage) holder_type(std::move(ptr), registered_class<T>::id, naked_ptr));
			}
		};


		template< class T, class Pointer, class Signature >
		struct construct :
			public construct_aux_helper <
			T,
			Pointer,
			Signature, typename meta::sub_range< Signature, 2, meta::size<Signature>::value >::type,
			typename meta::make_index_range<0, meta::size<Signature>::value - 2>::type >
		{
		};

	}	// namespace detail

}	// namespace luabind

# endif // LUABIND_DETAIL_CONSTRUCTOR_081018_HPP

