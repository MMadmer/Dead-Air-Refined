#pragma once

xr_vector<xr_string> BuildStackTrace(u16 maxFramesCount = 512);

#ifdef XR_PLATFORM_WINDOWS
struct AnonymousStackFrame
{
    xr_string module;
    uintptr_t moduleOffset{};
};

xr_vector<xr_string> BuildStackTrace(PCONTEXT threadCtx, u16 maxFramesCount);
xr_vector<AnonymousStackFrame> BuildAnonymousStackTrace(PCONTEXT threadCtx, u16 maxFramesCount);
#endif
