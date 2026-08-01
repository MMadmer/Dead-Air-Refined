include_guard()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output all libraries and executables to the runtime folder.
if (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(XRAY_OUTPUT_ARCH "x64")
else()
    set(XRAY_OUTPUT_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(XRAY_COMPILE_OUTPUT_FOLDER "${CMAKE_SOURCE_DIR}/bin/${XRAY_OUTPUT_ARCH}/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${XRAY_COMPILE_OUTPUT_FOLDER}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${XRAY_COMPILE_OUTPUT_FOLDER}")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${XRAY_COMPILE_OUTPUT_FOLDER}")

add_compile_definitions(
    # _DEBUG, DEBUG, MIXED, NDEBUG defines
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Debug,Mixed>:DEBUG>
    $<$<CONFIG:Mixed>:MIXED>
    $<$<CONFIG:Release,ReleaseMasterGold>:NDEBUG>
    # Tracy profiler
    $<$<BOOL:${XRAY_ENABLE_TRACY}>:TRACY_ENABLE>
    $<$<BOOL:${XRAY_ENABLE_TRACY}>:TRACY_NO_FRAME_IMAGE>
    # Luabind
    $<$<CONFIG:ReleaseMasterGold>:LUABIND_NO_EXCEPTIONS>
    $<$<CONFIG:ReleaseMasterGold>:LUABIND_NO_ERROR_CHECKING>
)

# Link-time optimization
include(CheckIPOSupported)
check_ipo_supported(RESULT LTO_IS_SUPPORTED)
if (LTO_IS_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASEMASTERGOLD ON)
endif()

# Main compiler settings
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    include(XRay.Compiler.MSVC)
elseif (CMAKE_CXX_COMPILER_ID MATCHES "GNU|LCC|Clang")
    include(XRay.Compiler.GNULike)
else()
    message(FATAL_ERROR "Unsupported or unknown compiler.")
endif()

# https://gitlab.kitware.com/cmake/cmake/-/issues/25650
if (CMAKE_VERSION VERSION_EQUAL "3.28.2" AND CMAKE_UNITY_BUILD)
    message(WARNING
        "In CMake 3.28.2, precompiled headers are broken when Unity build is enabled. \
        We have to disable Unity build. Please, update to CMake 3.28.3 or downgrade to 3.28.1."
    )
    set(CMAKE_UNITY_BUILD OFF)
endif()

query_git_info(XRAY_GIT_SHA XRAY_GIT_BRANCH)

message(VERBOSE "CMAKE_UNITY_BUILD:     ${CMAKE_UNITY_BUILD}")
message(STATUS  "CMAKE_PROJECT_VERSION: ${CMAKE_PROJECT_VERSION}")
message(STATUS  "XRAY_GIT_SHA:          ${XRAY_GIT_SHA}")
message(STATUS  "XRAY_GIT_BRANCH:       ${XRAY_GIT_BRANCH}")

message(STATUS "BUILD_SHARED_LIBS:     ${BUILD_SHARED_LIBS}")
message(STATUS "LTO_IS_SUPPORTED:      ${LTO_IS_SUPPORTED}")

message(DEBUG)
message(DEBUG "C++ Flags:")
message(DEBUG "           Global: ${CMAKE_CXX_FLAGS}")
message(DEBUG "            Debug: ${CMAKE_CXX_FLAGS_DEBUG}")
message(DEBUG "            Mixed: ${CMAKE_CXX_FLAGS_MIXED}")
message(DEBUG "          Release: ${CMAKE_CXX_FLAGS_RELEASE}")
message(DEBUG "ReleaseMasterGold: ${CMAKE_CXX_FLAGS_RELEASEMASTERGOLD}")

message(DEBUG)
message(DEBUG "C Flags:")
message(DEBUG "           Global: ${CMAKE_C_FLAGS}")
message(DEBUG "            Debug: ${CMAKE_C_FLAGS_DEBUG}")
message(DEBUG "            Mixed: ${CMAKE_C_FLAGS_MIXED}")
message(DEBUG "          Release: ${CMAKE_C_FLAGS_RELEASE}")
message(DEBUG "ReleaseMasterGold: ${CMAKE_C_FLAGS_RELEASEMASTERGOLD}")
message(DEBUG)

unset(LTO_IS_SUPPORTED)
unset(XRAY_OUTPUT_ARCH)
