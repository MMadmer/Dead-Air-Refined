////////////////////////////////////////////////////////////////////////////
//  Created     : 19.06.2018
//  Authors     : Xottab_DUTY (OpenXRay project)
//                FozeSt
//                Unfainthful
//
//  Copyright (C) GSC Game World - 2018
////////////////////////////////////////////////////////////////////////////
#pragma once

#include "xrEngine/ILoadingScreen.h"
#include "xrEngine/da_loading_presenter.h"
#include "xrUICore/Static/UIStatic.h"
#include "xrUICore/Windows/UIWindow.h"

class CApplication;

class UILoadingScreen final : public ILoadingScreen, public CUIWindow
{
    Lock loadingLock;
    bool alwaysShowStage;

    CUIProgressBar* loadingProgress;
    CUIStatic* loadingProgressPercent;
    CUIStatic* loadingLogo;

    CUIStatic* loadingStage;
    CUIStatic* loadingHeader;
    CUIStatic* loadingTipNumber;
    CUIStatic* loadingTip;
    // when the shown progress was eased last (da_media_time)
    u32 m_eased_at{};

    // What went to the loading screen's own presenter: each layer's description, picture and revision, by its
    // place in the screen; something changed since (a text, a logo, the screen shown again).
    struct Published
    {
        xr_string signature;
        u32 revision{};
        DaPicture pixels;
        std::shared_ptr<const xr_vector<DaPicture>> percents;
        // a texture that moves: where the static drew it and the part of it
        Fvector4 uv{};
        Irect drawn{};
    };
    xr_vector<Published> m_published;
    u32 m_revisions{};
    bool m_dirty{ true };

public:
    UILoadingScreen();

    void Initialize() override;

    void Show(bool show) override;

    [[nodiscard]]
    bool IsShown() const override;

    void Update(const int stagesCompleted, const int stagesTotal) override;
    void Draw() override;

    void SetLevelLogo(const char* name) override;
    void SetStageTitle(const char* title) override;
    void SetStageTip(const char* header, const char* tipNumber, const char* tip) override;

    bool Publish() override;
    [[nodiscard]]
    bool NeedsPublish() const override { return m_dirty; }

    pcstr GetDebugType() override { return "UILoadingScreen"; }
};

class NullLoadingScreen : public ILoadingScreen
{
public:
    void Initialize() override {}

    [[nodiscard]]
    bool IsShown() const override { return false; }
    void Show(bool /*status*/) override {}

    void Update(int /*stagesCompleted*/, int /*stagesTotal*/) override {}
    void Draw() override {}

    void SetLevelLogo(cpcstr /*name*/) override {}
    void SetStageTitle(cpcstr /*title*/) override {}
    void SetStageTip(cpcstr /*header*/, cpcstr /*tipNumber*/, cpcstr /*tip*/) override {}
};
