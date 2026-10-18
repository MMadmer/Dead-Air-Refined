// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The player's on/off switch (Options -> Game -> Interface): dar_damage_indicator
extern int g_dar_damage_indicator;

// Where a hit came from, the way shooters show it: an arc on a ring round the crosshair, on the side the hit came
// from, turning with the camera. Its strength - how bright, how wide, how long it stays - is the share of the
// health the hit took. Hits from about the same side make one arc. Its look is ui_theme.xml's "damage_arc".
class CDamageIndicator
{
public:
    // from: the world direction towards where the hit came from; lost: the share of the health it took
    void Add(const Fvector& from, float lost);
    void Render();
    void Clear() { m_marks.clear(); }

private:
    struct SMark
    {
        Fvector2 from; // horizontal, normalised: x, z
        float strength;
        float start;
        float life;
    };
    xr_vector<SMark> m_marks;
};
