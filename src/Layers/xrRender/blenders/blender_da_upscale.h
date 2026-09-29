// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// The passes of the upscale slot (r4_rendertarget_phase_upscale.cpp):
// 0 - the identity upscaler: generic0 stretched bilinearly into the display-size output;
// 1 - the depth upscale: the render-size scene depth into the display-size base depth (nearest);
// 2 - the reactive mask: how much the transparent passes changed each pixel of the scene.
class CBlender_da_upscale final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air upscale slot"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
