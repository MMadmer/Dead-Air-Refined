# A release-time tool, never shipped to a player: it cuts the chunk map published beside a content
# bundle, with xrContentSync's chunker, so the builder and every client cut at the same places.
add_executable(DarDelta)

target_sources(DarDelta PRIVATE DarDelta.cpp)

target_link_libraries(DarDelta PRIVATE xrContentSync bcrypt)

set_target_properties(DarDelta PROPERTIES
    FOLDER "Utilities"
)
