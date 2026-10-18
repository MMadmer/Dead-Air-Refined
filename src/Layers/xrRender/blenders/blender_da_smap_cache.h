// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// The lamp shadow cache's copy (da_smap_cache.cpp): a slot of the atlas written as depth into the light's rect of the
// live map, or the slot cleared to the far plane.
class CBlender_da_smap_cache final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air lamp shadow cache"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
