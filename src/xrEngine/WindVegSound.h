#pragma once

#include "xrSound/Sound.h"

// Wind-driven vegetation rustle (the Ghost of Yotei scheme: point-source one-shots spawned
// over clusters of vegetation, not loops). The travelling gust field decides WHERE: when a
// gust tongue passes over a tree crown or a grassy sector, that spot plays a rustle one-shot,
// scaled by the local wind strength. Grass whispers, bushes rustle, trees roar - and in a calm
// the world goes quiet. Grass and tree canopies carry DEDICATED sounds
// (sounds\dead_air_x64\grass_rustle / leaves_rustle); bushes keep the game's own
// actor-through-bush material pair, and any missing dedicated file falls back to it too.
class ENGINE_API CEffect_WindVeg
{
    struct SVoice
    {
        ref_sound snd;
        float busy_until{};
    };

    enum
    {
        type_grass,
        type_bush,
        type_tree,
        type_count
    };
    SVoice m_voices[type_count][3];
    // Per-type source files: dedicated assets for grass/trees, the material-pair collide
    // sounds for bushes (and as the fallback for everything).
    xr_vector<shared_str> m_files[type_count];
    bool m_inited{};
    bool m_sound_ok{};

    float m_next_think{};
    // Per-emitter retrigger cooldowns: 8 azimuth sectors for grass/bush + the tracked trees.
    float m_sector_cool[8]{};
    xr_vector<float> m_tree_cool;
    // Nearest-trees cache, refreshed on a slow timer.
    xr_vector<u32> m_near_trees;
    float m_next_tree_sort{};

    void lazy_init();
    bool play_one(int type, const Fvector& pos, float strength);

public:
    ~CEffect_WindVeg();
    void OnFrame();
    void OnLevelUnload();
};
