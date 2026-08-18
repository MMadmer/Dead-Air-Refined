add_executable(xr_3da)

if (WIN32)
    set_property(TARGET xr_3da PROPERTY WIN32_EXECUTABLE TRUE)
endif()

target_sources(xr_3da PRIVATE
    entry_point.cpp
    stdafx.h

    resource.h
    resource.rc

    splash_oxr.bmp
    stalker_cop.ico
    stalker_cs.ico
    stalker_soc.ico
)

if (MSVC)
    set(xr_3da_manifest "${CMAKE_CURRENT_SOURCE_DIR}/as-invoker-manifest-part.xml")

    # Merge the DPI settings into CMake's generated manifest so the executable keeps one RT_MANIFEST resource.
    target_link_options(xr_3da PRIVATE "/MANIFESTINPUT:${xr_3da_manifest}")
    set_property(TARGET xr_3da APPEND PROPERTY LINK_DEPENDS "${xr_3da_manifest}")
endif()

# The resource compiler gets no dependency scanning, so a version bump would relink the
# executable while leaving the stale VERSIONINFO in place. Diagnostic reports key off it.
set_property(
    SOURCE resource.rc
    APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/xrCore/ProductVersion.h"
)

set_property(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    PROPERTY
    VS_STARTUP_PROJECT xr_3da
)

target_include_directories(xr_3da
    PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(xr_3da
    PRIVATE
    xrCore
    xrAPI
    xrEngine
    xrRenderPC_R4
    xrGame
)

set_target_properties(xr_3da PROPERTIES
    OUTPUT_NAME "xrEngine"
    PDB_NAME "XR_3DA"
    PREFIX ""
)

if (WIN32)
    xray_stage_windows_runtime(xr_3da)
endif()

install(TARGETS xr_3da RUNTIME
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
