cmake_minimum_required(VERSION 3.23)

if (POLICY CMP0194)
    cmake_policy(SET CMP0194 NEW)
    set(CMAKE_POLICY_DEFAULT_CMP0194 NEW)
endif()
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PreProjectInit)

calculate_xray_build_id(XRAY_BUILD_ID)
project(OpenXRay
    DESCRIPTION "OpenXRay is an improved version of the X-Ray Engine, \
                the game engine used in the world-famous S.T.A.L.K.E.R. game series by GSC Game World."
    VERSION 1.6.02.${XRAY_BUILD_ID}
    HOMEPAGE_URL "https://github.com/OpenXRay/xray-16"
    LANGUAGES CXX C
)

# CMake 4.x can double-encode the Russian /showIncludes prefix.
# Ninja then silently loses all header dependencies.
set(_xray_msvc_russian_showincludes_mojibake "╨Я╤А╨╕╨╝╨╡╤З╨░╨╜╨╕╨╡: ╨▓╨║╨╗╤О╤З╨╡╨╜╨╕╨╡ ╤Д╨░╨╣╨╗╨░:  ")
if (MSVC AND CMAKE_GENERATOR MATCHES "^Ninja" AND
    CMAKE_CXX_CL_SHOWINCLUDES_PREFIX STREQUAL "${_xray_msvc_russian_showincludes_mojibake}")
    set(CMAKE_C_CL_SHOWINCLUDES_PREFIX "Примечание: включение файла:  ")
    set(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "Примечание: включение файла:  ")
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "Примечание: включение файла:  ")
endif()
unset(_xray_msvc_russian_showincludes_mojibake)

option(BUILD_SHARED_LIBS "Build shared libraries"     ${BUILD_SHARED_LIBS_DEFAULT_VALUE})

option(XRAY_USE_LUAJIT   "Use LuaJIT"                 ON)
option(XRAY_USE_ASAN     "Use AddressSanitizer"       OFF)
option(XRAY_ENABLE_TRACY "Enable tracy profiler"      OFF)

include(XRay.Build)
include(XRay.Packaging)
include(XRay.Dependencies)

set(_xray_ipo_release "${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE}")
set(_xray_ipo_release_master_gold "${CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASEMASTERGOLD}")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASEMASTERGOLD OFF)
add_subdirectory(Externals)
if (MEMORY_ALLOCATOR STREQUAL "mimalloc" AND NOT TARGET mimalloc)
    message(FATAL_ERROR "The mimalloc allocator was requested but its target is unavailable.")
endif()
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE "${_xray_ipo_release}")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASEMASTERGOLD "${_xray_ipo_release_master_gold}")
unset(_xray_ipo_release)
unset(_xray_ipo_release_master_gold)

add_subdirectory(src)
add_subdirectory(res)
add_subdirectory(misc)
