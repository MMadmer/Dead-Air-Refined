# The content system's shared core: manifest parsing and hashing, and nothing else.
#
# It is deliberately a static library with no xrCore dependency, because the same code has to
# run in three very different places - inside xrCore before the engine exists, inside xrGame,
# and inside the standalone updater executable. One implementation, three linkers.
add_library(xrContentSync STATIC)

target_sources(xrContentSync PRIVATE
    ContentHash.cpp
    ContentHash.h
    ContentManifest.cpp
    ContentManifest.h
)

target_include_directories(xrContentSync PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

set_target_properties(xrContentSync PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "xrContentSync"
)
