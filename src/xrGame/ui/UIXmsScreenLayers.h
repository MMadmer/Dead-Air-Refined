// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "xrUICore/Windows/UIWindow.h"
#include "xrCore/XMS/xms_core.h"

class CUIStatic;

// A module's screen on the screen: one window, its layers as statics in their order, so the game draws them like any
// window and the loading screen's own presenter finds them where it finds any other static.
class CUIXmsScreenLayers final : public CUIWindow
{
public:
    CUIXmsScreenLayers();
    ~CUIXmsScreenLayers() override;

    void Build(const XMS::Module& module, const XMS::ScreenDecl& screen, bool video_sound);
    void Clear();
    bool Built() const { return m_screen != nullptr; }

    // The display changed under a menu that stays open: its layers are measured again.
    void FollowDisplay();

    // A layer of this screen whose video keeps quiet: the screen has music of its own, or asks for no sound.
    bool Silent(const CUIWindow* window) const;

    pcstr GetDebugType() override { return "CUIXmsScreenLayers"; }

private:
    void AddLayer(const XMS::Module& module, const XMS::ScreenLayer& layer, bool video_sound);
    bool AddPage(const XMS::Module& module, const XMS::ScreenLayer& layer, CUIStatic* st, const Frect& place);

    const XMS::Module* m_module{};
    const XMS::ScreenDecl* m_screen{};
    bool m_video_sound{};
    u32 m_width{};
    u32 m_height{};
    xr_vector<const CUIWindow*> m_heard;
    xr_vector<xr_string> m_pictures;
};
