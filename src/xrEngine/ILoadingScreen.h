////////////////////////////////////////////////////////////////////////////
//  Created     : 19.06.2018
//  Authors     : Xottab_DUTY (OpenXRay project)
//                FozeSt
//                Unfainthful
//
//  Copyright (C) GSC Game World - 2018
////////////////////////////////////////////////////////////////////////////
#pragma once

class XR_NOVTABLE ILoadingScreen
{
public:
    virtual ~ILoadingScreen() = 0;

    virtual void Initialize() = 0;

    [[nodiscard]]
    virtual bool IsShown() const = 0;
    virtual void Show(bool show) = 0;

    virtual void Update(int stagesCompleted, int stagesTotal) = 0;
    virtual void Draw() = 0;

    virtual void SetLevelLogo(const char* name) = 0;
    virtual void SetStageTitle(const char* title) = 0;
    virtual void SetStageTip(const char* header, const char* tipNumber, const char* tip) = 0;

    // The loading screen's own presenter (da_loading_presenter.h): the screen's layers go to it, from a frame of
    // the game's - true when it took them. NeedsPublish: something it shows changed since.
    virtual bool Publish() { return false; }
    [[nodiscard]]
    virtual bool NeedsPublish() const { return false; }
};

inline ILoadingScreen::~ILoadingScreen() = default;
