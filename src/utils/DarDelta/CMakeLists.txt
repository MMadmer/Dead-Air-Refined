# A release-time tool, never shipped to a player: it builds the .darpatch between two revisions
# of a content bundle. Deliberately depends on nothing but the standard library and the OS, the
# same rule xrContentSync follows, so the format has exactly one writer and one reader.
add_executable(DarDelta)

target_sources(DarDelta PRIVATE DarDelta.cpp)

target_link_libraries(DarDelta PRIVATE bcrypt)

set_target_properties(DarDelta PROPERTIES
    FOLDER "Utilities"
)
