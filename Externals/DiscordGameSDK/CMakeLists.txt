add_library(DiscordGameSDK STATIC)

target_sources(DiscordGameSDK PRIVATE
    activity_manager.cpp
    activity_manager.h
    core.cpp
    core.h
    discord.h
    event.h
    ffi.h
    overlay_manager.cpp
    overlay_manager.h
    relationship_manager.cpp
    relationship_manager.h
    types.cpp
    types.h
    user_manager.cpp
    user_manager.h
)

target_include_directories(DiscordGameSDK
    PUBLIC
    "${CMAKE_SOURCE_DIR}/Externals"
)

target_link_libraries(DiscordGameSDK
    PUBLIC
    Discord::GameSDK
)
