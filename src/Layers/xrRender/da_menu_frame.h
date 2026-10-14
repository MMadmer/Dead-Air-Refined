// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The game's frame under the pause menu as a render target of its own, $user$game_frame, for a mod's menu
// shaders (docs/dead-air/MODDING.md): the menu asks for it as it opens over a level
// (IRender::CaptureMenuFrame), and the next frame drawn is copied on the GPU just before it is shown.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
void da_menu_frame_request();
// D3DXRenderBase::End, before the frame is presented: copies the asked-for frame.
void da_menu_frame_capture();
// CRender::destroy: the texture goes while the resource manager is alive.
void da_menu_frame_destroy();
} // namespace xray::render::RENDER_NAMESPACE
