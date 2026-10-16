#pragma once

#include "xrEngine/EngineAPI.h"

// The OpenGL renderer is removed from the build. Three renderer modules compile the same render
// code: R4 (DirectX 11), R5 (DirectX 12, through xrD3D12) and R6 (Vulkan, through xrVulkan).
#ifdef XRAY_STATIC_BUILD
#    define XRRENDER_R4_API
#    define XRRENDER_R5_API
#    define XRRENDER_R6_API
#else
#    ifdef XRRENDER_R4_EXPORTS
#        define XRRENDER_R4_API XR_EXPORT
#    else
#        define XRRENDER_R4_API XR_IMPORT
#    endif
#    ifdef XRRENDER_R5_EXPORTS
#        define XRRENDER_R5_API XR_EXPORT
#    else
#        define XRRENDER_R5_API XR_IMPORT
#    endif
#    ifdef XRRENDER_R6_EXPORTS
#        define XRRENDER_R6_API XR_EXPORT
#    else
#        define XRRENDER_R6_API XR_IMPORT
#    endif
#endif

namespace xray::render
{
#ifdef XR_PLATFORM_WINDOWS
namespace render_r4
{
XRRENDER_R4_API RendererModule* GetRendererModule();
}
namespace render_r5
{
XRRENDER_R5_API RendererModule* GetRendererModule();
}
namespace render_r6
{
XRRENDER_R6_API RendererModule* GetRendererModule();
}
#endif
} // namespace xray::render
