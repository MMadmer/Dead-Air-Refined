add_subdirectory(xrAPI)

if (WIN32)
    add_subdirectory(xrRenderPC_R4)
    # DirectX 12: the xrD3D12 library and the renderer module built on it from the R4 sources.
    add_subdirectory(xrRenderDX12)
    add_subdirectory(xrRenderPC_R5)
    # Vulkan: the xrVulkan library and the renderer module built on it from the R4 sources.
    add_subdirectory(xrRenderVK)
    add_subdirectory(xrRenderPC_R6)
endif()

# The OpenGL renderer is removed from the build: only R4 is played and maintained, and
# xrRender_GL was a static import of the exe, so its DLL was required for startup even on
# DX11 while its code never ran. The Layers/xrRenderPC_GL and Layers/xrRenderGL sources
# stay in the tree, they are simply not built.
