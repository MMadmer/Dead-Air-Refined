#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// Ground-truth based ambient occlusion (ported from IX-Ray):
// E[0] renders view-z + raw AO into $user$gtao_0, E[1] guided-filters it into
// $user$ssao_temp for combine_1. With compute set, the same two as compute passes
// (gtao_render.cs, gtao_filter.cs: the DirectX 12 async path) writing through UAVs.
class CBlender_gtao : public IBlender
{
public:
    bool compute{};

    virtual LPCSTR getComment() { return "INTERNAL: ground-truth ambient occlusion"; }
    virtual BOOL canBeDetailed() { return FALSE; }
    virtual BOOL canBeLMAPped() { return FALSE; }
    virtual void Compile(CBlender_Compile& C);
};
} // namespace xray::render::RENDER_NAMESPACE
