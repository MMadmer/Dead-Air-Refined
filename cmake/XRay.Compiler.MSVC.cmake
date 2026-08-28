include_guard()

# The MSVC compiler settings:
# Set properties:
set(CMAKE_VS_USE_DEBUG_LIBRARIES "$<CONFIG:Debug>")

# Clear predefined flags which we going to define ourselves
string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS}) # exceptions
string(REGEX REPLACE "/Z(7|i|I)" "" CMAKE_CXX_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG}) # debug information format

# Enable standard C++ exceptions everywhere except ReleaseMasterGold
add_compile_options($<$<NOT:$<CONFIG:ReleaseMasterGold>>:/EHsc>)

# Disable MS STL exceptions on ReleaseMasterGold
add_compile_definitions($<$<CONFIG:ReleaseMasterGold>:_HAS_EXCEPTIONS=0>)

# Enable debug information for all configurations
add_compile_options(/Zi)
add_compile_options($<$<CONFIG:Mixed>:/Od> $<$<CONFIG:Mixed>:/Ob0>)
add_compile_options($<$<CONFIG:ReleaseMasterGold>:/O2> $<$<CONFIG:ReleaseMasterGold>:/Ob2>)

# Enable SSE2 for 32-bit build
# (on x64 it's always enabled and produces error if try to to enable it)
add_compile_options($<$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>:/arch:SSE2>)

# Warning level: CMP0092 (NEW) strips the historical /W3 default, which silently left the whole
# project at /W1 - uninitialized-variable diagnostics (C4700 family) never printed. No /WX here;
# CI is the place for that.
add_compile_options(/W3)

# Disable specific warnings
add_compile_options(
    /wd4201 # nonstandard extension used : nameless struct/union
    /wd4251 # class 'x' needs to have dll-interface to be used by clients of class 'y'
    /wd4275 # non dll-interface class 'x' used as base for dll-interface class 'y'
)

# AddressSanitizer: the XRAY_USE_ASAN option existed but only the GNU-like compiler file consumed
# it, so -DXRAY_USE_ASAN=ON on MSVC silently built nothing special. Prerequisites handled here:
# /fsanitize=address is incompatible with /RTC, /GL(+LTCG) and incremental linking; the mimalloc
# allocator hides allocations from ASan, so build with -DMEMORY_ALLOCATOR=standard as well.
if (XRAY_USE_ASAN)
    add_compile_options(/fsanitize=address)
    string(REGEX REPLACE "/RTC(su|[1su])" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
    add_link_options(/INCREMENTAL:NO)
    if (NOT MEMORY_ALLOCATOR STREQUAL "standard")
        message(WARNING "XRAY_USE_ASAN works best with -DMEMORY_ALLOCATOR=standard (mimalloc hides allocations from ASan)")
    endif()
endif()

# The MSVC linker settings:
add_link_options("/LARGEADDRESSAWARE")
add_link_options($<$<NOT:$<CONFIG:ReleaseMasterGold>>:/DEBUG:FULL>)
add_link_options($<$<CONFIG:Release,ReleaseMasterGold>:/OPT:REF> $<$<CONFIG:Release,ReleaseMasterGold>:/OPT:ICF>)

set(XRAY_DISABLE_WARNINGS "/w")
