# xrVulkan: the native Vulkan backend behind the Direct3D 11 interfaces the renderer uses. Linked
# statically into xrRender_R6.dll. vulkan-1.dll is loaded at runtime, so the module (a static import of
# the exe) still loads where Vulkan is absent. The Vulkan headers come with SDL (Externals/SDL).
add_library(xrVulkan STATIC)

target_sources(xrVulkan PRIVATE
    vk_api.h
    vk_layer.h
    vk_loader.h
    vk_loader.cpp
    vk_infra.cpp
    vk_spirv.cpp
    vk_pipeline.cpp
    vk_device.cpp
    vk_objects.cpp
    vk_context_state.cpp
    vk_context_ops.cpp
    vk_swapchain.cpp
    vk_native.cpp
    ../xrRender/present_pacing.h
)

target_include_directories(xrVulkan
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/Externals/SDL/src/video/khronos"
)

target_compile_features(xrVulkan PUBLIC cxx_std_20)

target_compile_definitions(xrVulkan PRIVATE NOMINMAX)

target_link_libraries(xrVulkan
    PUBLIC
    dxguid
    dxgi
)

set_target_properties(xrVulkan PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    # Anonymous-namespace helpers of the separate files must not meet in one unity source.
    UNITY_BUILD OFF
    FOLDER "Layers"
)
