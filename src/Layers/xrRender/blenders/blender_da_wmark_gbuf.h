// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// The decals' share of the G-buffer (WallmarksEngine.cpp, da_wmark_gbuf.ps): a wallmark's texture shapes the surface's
// normal and gloss under it, blended by its coverage. Created per wallmark texture.
class CBlender_da_wmark_gbuf final : public IBlender
{
public:
    LPCSTR getComment() override { return "INTERNAL: Dead Air decal G-buffer"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override;
};
} // namespace xray::render::RENDER_NAMESPACE
