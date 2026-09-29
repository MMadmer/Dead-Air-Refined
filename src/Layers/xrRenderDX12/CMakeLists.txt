# xrD3D12: the native Direct3D 12 backend behind the Direct3D 11 interfaces the renderer uses.
# Linked statically into xrRender_R5.dll. d3d12.dll and dxgi.dll are loaded at runtime, so the
# module (a static import of the exe) still loads where Direct3D 12 is absent.
add_library(xrD3D12 STATIC)

target_sources(xrD3D12 PRIVATE
    d12_api.h
    d12_layer.h
    d12_pacing.h
    d12_infra.cpp
    d12_device.cpp
    d12_objects.cpp
    d12_context_state.cpp
    d12_context_ops.cpp
    d12_swapchain.cpp
    # DirectX 12 feature work (docs/dead-air/dx12-features/PLAN.md 2.3)
    d12_native.cpp
    d12_async.cpp
    d12_dxc.cpp
    d12_vrs.cpp
    d12_mesh.cpp
    d12_texheap.cpp
)

target_include_directories(xrD3D12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_features(xrD3D12 PUBLIC cxx_std_20)

target_compile_definitions(xrD3D12 PRIVATE NOMINMAX)

target_link_libraries(xrD3D12
    PUBLIC
    dxguid
    d3dcompiler
    # CoTaskMemFree (DXC's version strings)
    ole32
)

set_target_properties(xrD3D12 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    # Anonymous-namespace helpers of the separate files must not meet in one unity source.
    UNITY_BUILD OFF
    FOLDER "Layers"
)
