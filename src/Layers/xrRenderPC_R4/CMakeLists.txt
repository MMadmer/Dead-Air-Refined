add_library(xrRenderPC_R4)

include(sources.cmake)
target_sources(xrRenderPC_R4 PRIVATE ${XRAY_RENDER_R4_SOURCES})

target_include_directories(xrRenderPC_R4
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/Layers/xrRender_R2"
    "${CMAKE_SOURCE_DIR}/Externals/imgui"
    "${CMAKE_SOURCE_DIR}/Externals/OpenAutomate/inc"
    "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK"
    "${CMAKE_SOURCE_DIR}/sdk/include"
    "${CMAKE_SOURCE_DIR}/sdk/include/DirectXMesh"
    "${CMAKE_SOURCE_DIR}/sdk/include/nvapi"
)

target_link_libraries(xrRenderPC_R4
    PRIVATE
    xrCore
    xrCDB
    xrEngine
    xrMaterialSystem
    xrParticles
    xrScriptEngine
    xrAPI
    xrMiscMath
    DirectXMath
    DirectXTex
    NVIDIA::NvAPI
    AMD::AGS
    d3dcompiler
    dxguid
    dwmapi
    gdi32
)

target_compile_definitions(xrRenderPC_R4
    PRIVATE
    XRRENDER_R4_EXPORTS
    RENDER_NAMESPACE=render_r4
    USE_DX11
)

set_target_properties(xrRenderPC_R4 PROPERTIES
    OUTPUT_NAME "xrRender_R4"
    PREFIX ""
)

target_precompile_headers(xrRenderPC_R4
    PRIVATE
    stdafx.h
)

if (BUILD_SHARED_LIBS)
    install(TARGETS xrRenderPC_R4 LIBRARY
        DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()

unset(XRAY_RENDER_R4_SOURCES)
