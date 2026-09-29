# The DirectX 12 renderer module: the R4 sources compiled against xrD3D12, which implements the
# D3D11 interfaces the render code uses on top of a native D3D12 device. USE_DX11 and RENDER R_R4
# stay: the render code is the same; XR_D3D12 marks the few places that differ.
add_library(xrRenderPC_R5)

include(../xrRenderPC_R4/sources.cmake)
list(FILTER XRAY_RENDER_R4_SOURCES EXCLUDE REGEX "/xrRender_R4\\.cpp$")
target_sources(xrRenderPC_R5 PRIVATE
    ${XRAY_RENDER_R4_SOURCES}
    stdafx.h
    xrRender_R5.cpp
)

target_include_directories(xrRenderPC_R5
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/Layers/xrRenderPC_R4"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/Layers/xrRender_R2"
    "${CMAKE_SOURCE_DIR}/Externals/imgui"
    "${CMAKE_SOURCE_DIR}/Externals/OpenAutomate/inc"
    "${CMAKE_SOURCE_DIR}/Externals/AGS_SDK"
    "${CMAKE_SOURCE_DIR}/sdk/include"
    "${CMAKE_SOURCE_DIR}/sdk/include/DirectXMesh"
    "${CMAKE_SOURCE_DIR}/sdk/include/nvapi"
)

target_link_libraries(xrRenderPC_R5
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
    AMD::FidelityFXAPI
    NVIDIA::Streamline
    xrD3D12
    d3dcompiler
    dxguid
    dwmapi
    gdi32
    # [dx12:core] begin libraries
    # dstorage.h for da_dstorage.cpp; the runtime is loaded by path, there is no import library.
    Microsoft::DirectStorageHeaders
    # [dx12:core] end libraries
    # [dx12:ds] begin libraries
    # [dx12:ds] end libraries
    # [dx12:rt] begin libraries
    # [dx12:rt] end libraries
)

target_compile_definitions(xrRenderPC_R5
    PRIVATE
    XRRENDER_R5_EXPORTS
    RENDER_NAMESPACE=render_r5
    USE_DX11
    XR_D3D12
)

set_target_properties(xrRenderPC_R5 PROPERTIES
    OUTPUT_NAME "xrRender_R5"
    PREFIX ""
)

target_precompile_headers(xrRenderPC_R5
    PRIVATE
    stdafx.h
)

if (BUILD_SHARED_LIBS)
    install(TARGETS xrRenderPC_R5 LIBRARY
        DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()

unset(XRAY_RENDER_R4_SOURCES)
