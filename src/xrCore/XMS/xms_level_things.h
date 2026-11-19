// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS level things: what a module adds to a level of the game beside its picture, its collision
// and its spawns - sounds that play at a place, particle effects, lights, environment modifiers,
// glows, fog volumes and sound environments (`levels\<level>\overlay.things`, text), read from
// every applying module in module order, like overlay.xcform. Nothing of the level's own is
// replaced: a module's things stand beside the level's. Contract:
// docs/dead-air/XMS_ARCHITECTURE.md 8.5.

#include "xrCore/xrCore.h"

namespace XMS
{
struct LevelSound
{
    xr_string name;
    Fvector position{};
    float volume{1.f};
    float frequency{1.f};
    // game time of the day it plays in, milliseconds; both 0: always
    Ivector2 active{0, 0};
    // how long it plays and how long it rests, milliseconds, each picked between the two; both
    // rests 0: it loops
    Ivector2 play{0, 0};
    Ivector2 pause{0, 0};
};

struct LevelParticles
{
    xr_string name;
    Fvector position{};
    // heading, pitch, bank, radians
    Fvector rotation{};
};

struct LevelLight
{
    bool spot{};
    Fvector position{};
    Fvector direction{0.f, -1.f, 0.f};
    Fvector color{1.f, 1.f, 1.f};
    float range{8.f};
    // radians, a spot's whole angle
    float cone{1.5f};
    bool shadows{};
};

struct LevelEnv
{
    Fvector position{};
    float radius{10.f};
    float power{1.f};
    float far_plane{};
    Fvector fog_color{};
    float fog_density{};
    Fvector ambient{};
    Fvector sky_color{};
    Fvector hemi_color{};
    // which of the values above it changes: far plane 1, fog colour 2, fog density 4, ambient 8,
    // sky colour 16, hemisphere colour 32
    u16 use{};
};

// A glow: a picture that always faces the viewer, fading in while its place is in sight.
struct LevelGlow
{
    Fvector position{};
    float radius{0.5f};
    xr_string texture;
    // empty: the lens flares' shader
    xr_string shader;
    Fvector color{1.f, 1.f, 1.f};
    // the same size on the screen at any distance: `radius` is then a share of half the screen's
    // height
    bool fixed{};
};

// A box in the level: the unit cube (-0.5..0.5) scaled by `size`, turned and moved.
struct LevelBox
{
    Fvector position{};
    // heading, pitch, bank, radians
    Fvector rotation{};
    Fvector size{1.f, 1.f, 1.f};

    Fmatrix transform() const
    {
        Fmatrix turn, scale, out;
        turn.setHPB(rotation.x, rotation.y, rotation.z);
        scale.scale(size.x, size.y, size.z);
        out.mul_43(turn, scale);
        out.c.set(position.x, position.y, position.z);
        return out;
    }
};

// A volume of fog that flows: the box it fills, the game's profile it flows by, the boxes it
// flows around.
struct LevelFog
{
    xr_string profile;
    LevelBox box;
    xr_vector<LevelBox> occluders;
};

// A box with the sound environment inside it and the one above it.
struct LevelSoundEnv
{
    LevelBox box;
    xr_string inner;
    xr_string outer;
};

struct LevelThings
{
    xr_vector<LevelSound> sounds;
    xr_vector<LevelParticles> particles;
    xr_vector<LevelLight> lights;
    xr_vector<LevelEnv> envs;
    xr_vector<LevelGlow> glows;
    xr_vector<LevelFog> fogs;
    xr_vector<LevelSoundEnv> sound_envs;

    bool empty() const
    {
        return sounds.empty() && particles.empty() && lights.empty() && envs.empty() && glows.empty() &&
            fogs.empty() && sound_envs.empty();
    }
};

// `level` is the level's folder name. A file that is too large, or a block that does not read,
// is left out with one log line.
XRCORE_API void ReadLevelThings(pcstr level, LevelThings& out);
// The same for the level that is loading: its folder is what `$level$` names.
XRCORE_API void ReadLevelThings(LevelThings& out);
} // namespace XMS
