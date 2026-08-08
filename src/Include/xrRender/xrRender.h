#pragma once

#include "xrEngine/EngineAPI.h"

// The OpenGL renderer is removed from the build: R4 is the only renderer module.
#ifdef XRAY_STATIC_BUILD
#    define XRRENDER_R4_API
#else
#    ifdef XRRENDER_R4_EXPORTS
#        define XRRENDER_R4_API XR_EXPORT
#    else
#        define XRRENDER_R4_API XR_IMPORT
#    endif
#endif

namespace xray::render
{
#ifdef XR_PLATFORM_WINDOWS
namespace render_r4
{
XRRENDER_R4_API RendererModule* GetRendererModule();
}
#endif
} // namespace xray::render
