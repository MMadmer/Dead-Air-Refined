set(SDL_SHARED ON CACHE BOOL "Build the shared SDL2 library" FORCE)
set(SDL_STATIC OFF CACHE BOOL "Do not build the unused static SDL2 library" FORCE)
set(SDL_TEST OFF CACHE BOOL "Do not build the SDL2 test library" FORCE)
set(SDL_TESTS OFF CACHE BOOL "Do not build the SDL2 test programs" FORCE)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "Suppress deprecation warnings from pinned dependencies" FORCE)
add_subdirectory(SDL EXCLUDE_FROM_ALL)
unset(CMAKE_WARN_DEPRECATED CACHE)
add_subdirectory(DiscordGameSDK)

set(MI_BUILD_SHARED OFF CACHE BOOL "Do not build the mimalloc shared library" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "Build the mimalloc static library" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "Do not build the mimalloc object library" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "Do not build mimalloc tests" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "Do not override the process allocator" FORCE)
add_subdirectory(mimalloc EXCLUDE_FROM_ALL)
add_library(mimalloc ALIAS mimalloc-static)

set(BUILD_XDSP OFF CACHE BOOL "Do not build DirectXMath extensions" FORCE)
set(BUILD_SHMATH OFF CACHE BOOL "Do not build DirectXMath extensions" FORCE)
add_subdirectory(DirectXMath EXCLUDE_FROM_ALL)
add_library(Microsoft::DirectXMath ALIAS DirectXMath)

set(_xray_build_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build DirectXTex as a static library" FORCE)
set(BUILD_TOOLS OFF CACHE BOOL "Do not build DirectXTex tools" FORCE)
set(BUILD_SAMPLE OFF CACHE BOOL "Do not build DirectXTex samples" FORCE)
set(BUILD_DX12 OFF CACHE BOOL "Do not build unused DirectXTex D3D12 support" FORCE)
add_subdirectory(DirectXTex EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS "${_xray_build_shared_libs}" CACHE BOOL "Build engine libraries as shared libraries" FORCE)
unset(_xray_build_shared_libs)

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Do not build zlib examples" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "Do not build zlib tests" FORCE)
add_subdirectory(zlib EXCLUDE_FROM_ALL)

set(MINIZIP_BUILD_SHARED OFF CACHE BOOL "Do not build shared minizip" FORCE)
set(MINIZIP_BUILD_STATIC ON CACHE BOOL "Build static minizip" FORCE)
set(MINIZIP_BUILD_TESTING OFF CACHE BOOL "Do not build minizip tests" FORCE)
set(MINIZIP_INSTALL OFF CACHE BOOL "Do not install minizip" FORCE)
add_subdirectory(zlib/contrib/minizip EXCLUDE_FROM_ALL)

if (XRAY_USE_LUAJIT)
    if (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(LUAJIT_DISABLE_GC64 ON CACHE BOOL "Preserve the engine's x64 LuaJIT ABI" FORCE)
    endif()
    add_subdirectory(LuaJIT-proj)
else()
    find_package(Lua51 REQUIRED)
endif()

add_subdirectory(luabind)
set_target_properties(xrLuabind PROPERTIES OUTPUT_NAME "luabind")
add_subdirectory(xrLuaFix)
add_subdirectory(OPCODE)
add_subdirectory(ode)

# Silence all warnings for external libraries
add_compile_options(${XRAY_DISABLE_WARNINGS})

add_subdirectory(GameSpy)
add_subdirectory(imgui-proj)

if (NOT TARGET xrLuabind)
    message(FATAL_ERROR
        "You probably have downloaded OpenXRay sources as ZIP archive, you can't do that. Use git to clone the repository.\n"
        "Read the build instructions: https://github.com/OpenXRay/xray-16/wiki"
    )
endif()
