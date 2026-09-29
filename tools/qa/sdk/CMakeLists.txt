# Smoke test of the vendored upscaler SDKs, built outside the engine: tools\qa\sdk\Run-SdkSmoke.ps1.
cmake_minimum_required(VERSION 3.23)
project(xr_sdk_smoke CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The engine's dependency file resolves its paths from the repository root.
get_filename_component(XRAY_REPO "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(CMAKE_SOURCE_DIR "${XRAY_REPO}")
include("${XRAY_REPO}/cmake/XRay.Dependencies.cmake")

add_executable(sdk_smoke
    sdk_smoke.cpp
    sdk_smoke_dx12.cpp
    sdk_headers_dx11.cpp
    sdk_headers_dx12.cpp
)
target_compile_options(sdk_smoke PRIVATE /W3 /WX /permissive-)
target_link_libraries(sdk_smoke PRIVATE NVIDIA::Streamline AMD::FidelityFXAPI AMD::FidelityFXDX11 d3d11 d3d12 dxgi)
add_custom_command(TARGET sdk_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${XRAY_STREAMLINE_RUNTIME_FILES}
        ${XRAY_FIDELITYFX_RUNTIME_FILES}
        "$<TARGET_FILE_DIR:sdk_smoke>"
    COMMAND_EXPAND_LISTS
    VERBATIM
)

# The ffx:: C++ helpers through the delay-loaded loader import library.
add_executable(sdk_loader_smoke sdk_loader_smoke.cpp)
target_compile_options(sdk_loader_smoke PRIVATE /W3 /WX /permissive-)
target_link_libraries(sdk_loader_smoke PRIVATE AMD::FidelityFXLoader d3d12)
add_dependencies(sdk_loader_smoke sdk_smoke)
