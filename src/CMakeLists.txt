add_library(xrLuabind)

target_sources(xrLuabind PRIVATE
	class.cpp
	class_info.cpp
	class_registry.cpp
	class_rep.cpp
	create_class.cpp
	error.cpp
	exception_handler.cpp
	function.cpp
	function_introspection.cpp
	headertest.cpp
	inheritance.cpp
	link_compatibility.cpp
	memory.cpp
	object_rep.cpp
	open.cpp
	operator.cpp
	pcall.cpp
	scope.cpp
	set_package_preload.cpp
	stack_content_by_name.cpp
	weak_ref.cpp
	wrapper_base.cpp
)

target_include_directories(xrLuabind
	PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(xrLuabind
	PUBLIC
	xrLuabindApi
)

if (XRAY_USE_LUAJIT)
    target_link_libraries(xrLuabind PUBLIC xrLuaJIT)
else()
    find_package(Lua REQUIRED)
    target_link_libraries(xrLuabind PUBLIC ${LUA_LIBRARIES})
endif()

target_compile_definitions(xrLuabind
	PUBLIC
	$<$<BOOL:${BUILD_SHARED_LIBS}>:LUABIND_DYNAMIC_LINK>
)

set_target_properties(xrLuabind PROPERTIES
	PREFIX ""
)

if (BUILD_SHARED_LIBS)
	install(TARGETS xrLuabind LIBRARY
		DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	)
endif()
