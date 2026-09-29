// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The wind the vegetation and water vertex shaders read, as the previous frame drew with it.
// The motion vectors deform every vertex twice, now and one frame back
// (docs/dead-air/DX12_RENDERER.md, 3.4); the binders of the *_prev constants read this.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
struct da_wind_frame
{
    Fvector4 field{}; // da_wind_field: scroll offset of the gust field, strength, gustiness
    Fmatrix wm_pos[2]{}; // the wind motor rows, untransposed, as CEnvironment packs them
    Fmatrix wm_par[2]{};
    float wm_active{}; // live motor rows
    float time{}; // timers.x, Device.fTimeGlobal (the water's bob)
};

// The previous frame's wind. This frame's on a history reset and after a frame that drew
// nothing reading it (a stale snapshot would move everything by the gap). The first caller of
// a frame publishes it; every render thread of the frame reads the same record.
const da_wind_frame& da_wind_prev();
} // namespace xray::render::RENDER_NAMESPACE
