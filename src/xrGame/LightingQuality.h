#pragma once

#include "xrEngine/XR_IOConsole.h"

namespace GameLighting
{
inline u32 Quality()
{
    static u32 cachedFrame = u32(-1);
    static u32 cachedQuality = 4;
    if (cachedFrame == Device.dwFrame)
        return cachedQuality;

    cachedFrame = Device.dwFrame;
    u32 quality = 4;
    if (!Console || !Console->GetTokenValue("r2_lighting_quality", quality))
        return cachedQuality = 4;

    return cachedQuality = _min(quality, 4u);
}
}
