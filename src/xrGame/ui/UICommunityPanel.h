// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// dar_community_links: the panel shown (1) or not
extern int g_dar_community_links;

// The community panel of the main menu and the pause menu: the page RemoteConfig takes from the mod's repository,
// laid over the menu - 768 CSS pixels high on any screen, as wide as the screen's aspect makes it -, drawn where
// it draws something. A click on one of its links opens the address in the browser; a click elsewhere is the menu's.
class CUICommunityPanel
{
public:
    CUICommunityPanel();
    ~CUICommunityPanel();

    // the page laid out anew when another came or the screen changed, its picture made anew when the cursor
    // changed what it hovers; then the picture over the menu
    void Draw();
    // not drawn: nothing hovered any longer
    void Hide();

    // the left button over a link: true, and the menu does not see the click
    bool OnMousePress();
    bool OnMouseRelease();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
