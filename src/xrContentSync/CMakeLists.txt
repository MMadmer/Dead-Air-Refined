# The content system's shared core: the manifest format, hashing, the state files, and the
# resolve/fetch/commit pipeline.
#
# It is deliberately a static library with no xrCore dependency, because the same code has to
# run in three very different places - inside xrCore before the engine exists, inside xrGame,
# and inside the standalone updater executable, which the packaging script compiles straight
# from these sources rather than linking a CMake artefact. One implementation, three linkers.
add_library(xrContentSync STATIC)

target_sources(xrContentSync PRIVATE
    ContentCommit.cpp
    ContentCommit.h
    ContentDownload.cpp
    ContentDownload.h
    ContentHash.cpp
    ContentHash.h
    ContentManifest.cpp
    ContentManifest.h
    ContentPaths.h
    ContentResolver.cpp
    ContentResolver.h
    ContentState.cpp
    ContentState.h
)

target_include_directories(xrContentSync PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

set_target_properties(xrContentSync PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    FOLDER "xrContentSync"
)
