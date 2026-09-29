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

// Temporal AA: one resolve pass blending the current (jittered) LDR frame with the reprojected
// history under a neighbourhood clamp; the motion vectors reproject when they exist.
class CBlender_taa final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air camera TAA"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
