// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// The motion blur's passes (r4_rendertarget_phase_da_mblur.cpp): 2 the motion and the depth of every render pixel,
// 0 the longest motion of each tile, 1 of each tile and its neighbours. combine_2 gathers along it.
class CBlender_da_mblur final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air motion blur tiles"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
