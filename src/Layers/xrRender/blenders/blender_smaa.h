#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBlender_smaa final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air SMAA 1x"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};

// Camera-reprojection temporal AA: one resolve pass blending the current LDR frame with
// the reprojected history under a neighbourhood clamp (no jitter, no object velocity).
class CBlender_taa final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air camera TAA"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
