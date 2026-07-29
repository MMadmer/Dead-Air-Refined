#include "stdafx.h"

namespace xray::render::RENDER_NAMESPACE
{
static constexpr u32 GroupTexelDimension = 56;
static constexpr u32 GroupTexelOverlap = 12;
static constexpr u32 GroupTexelDimensionAfterOverlap = GroupTexelDimension - 2 * GroupTexelOverlap;

void CRenderTarget::phase_hdao()
{
    if (ps_r_ssao <= 0)
        return;

    SPass& P = *s_hdao_cs->E[0]->passes[0];
    RCache.set_States(P.state);
    RCache.set_CS(P.cs);
    RCache.set_Constants(P.constants);
    RCache.set_Textures(P.T);

    RCache.set_RT(nullptr, 0);
    RCache.set_RT(nullptr, 1);
    RCache.set_RT(nullptr, 2);
    RCache.set_RT(nullptr, 3);

    u32 UAVInitialCounts = 1;
    auto* context = HW.get_context(RCache.context_id);
    context->CSSetUnorderedAccessViews(0, 1, &rt_ssao_temp->pUAView, &UAVInitialCounts);

    const u32 groupsX = (dwWidth[RCache.context_id] + GroupTexelDimensionAfterOverlap - 1) /
        GroupTexelDimensionAfterOverlap;
    const u32 groupsY = (dwHeight[RCache.context_id] + GroupTexelDimensionAfterOverlap - 1) /
        GroupTexelDimensionAfterOverlap;
    RCache.Compute(groupsX, groupsY, 1);

    ID3D11UnorderedAccessView* uav = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &uav, &UAVInitialCounts);
    RCache.clear_CS_resources();
}
} // namespace xray::render::RENDER_NAMESPACE
