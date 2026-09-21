// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// Applies the first-person self-shadow map to the HUD pixels of the accumulator.
class CBlender_hud_shadow : public IBlender
{
public:
    virtual LPCSTR getComment() { return "INTERNAL: first-person self-shadow"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};
} // namespace xray::render::RENDER_NAMESPACE
