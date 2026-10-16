# The Vulkan renderer module: the R4 sources compiled against xrVulkan, which implements the D3D11
# interfaces the render code uses on top of a native Vulkan 1.3 device. USE_DX11 and RENDER R_R4 stay:
# the render code is the same; XR_VULKAN marks the few places that differ, XR_NATIVE the ones it shares
# with the DirectX 12 module (DXC shaders, the extension calls of the layers). xrD3D12 is linked for its
# DXC service and the DXIL reflection the constant tables are built from; d3d12.dll is never loaded here.
add_library(xrRenderPC_R6)

include(../xrRenderPC_R4/sources.cmake)
list(FILTER XRAY_RENDER_R4_SOURCES EXCLUDE REGEX "/xrRender_R4\\.cpp$")
target_sources(xrRenderPC_R6 PRIVATE
    ${XRAY_RENDER_R4_SOURCES}
    stdafx.h
    xrRender_R6.cpp
)

target_include_directories(xrRenderPC_R6
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

target_link_libraries(xrRenderPC_R6
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
    xrVulkan
    xrD3D12
    d3dcompiler
    dxguid
    dwmapi
    gdi32
    Microsoft::DirectStorageHeaders
)

target_compile_definitions(xrRenderPC_R6
    PRIVATE
    XRRENDER_R6_EXPORTS
    RENDER_NAMESPACE=render_r6
    USE_DX11
    XR_VULKAN
    XR_NATIVE
)

set_target_properties(xrRenderPC_R6 PROPERTIES
    OUTPUT_NAME "xrRender_R6"
    PREFIX ""
)

target_precompile_headers(xrRenderPC_R6
    PRIVATE
    stdafx.h
)

if (BUILD_SHARED_LIBS)
    install(TARGETS xrRenderPC_R6 LIBRARY
        DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    )
endif()

unset(XRAY_RENDER_R4_SOURCES)
