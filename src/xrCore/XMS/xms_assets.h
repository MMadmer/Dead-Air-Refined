// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS asset descriptions - what a module says about its own textures, sounds and models in plain
// Lua data beside the game's containers: gamedata\textures\**\*.texasset beside <name>.dds,
// gamedata\sounds\**\*.sndasset beside <name>.ogg, gamedata\meshes\**\*.meshasset beside
// <name>.ogf. Read once when the modules are mounted; the texture descriptions, the sound
// sources and the model pool apply them over what their own files say.
// Contract: docs/dead-air/MODDING.md, "A module's models, textures and sounds".

#include "xrCore/xrCore.h"
#include "xrCommon/xr_map.h"
#include "xrCommon/xr_vector.h"

namespace XMS::Assets
{
// A key the description leaves out is the game file's: each part says whether it was stated.
struct Texture
{
    xr_string file; // <module>\<path inside it>, for the log
    bool described{true}; // false for `use` ui and cube: the game describes no such texture
    s8 bump{-1}; // -1 unstated, 0 none, 1 a bump pair
    xr_string bump_name; // empty: <name>_bump
    bool parallax{false};
    s8 detail{-1}; // -1 unstated, 0 none, 1 a detail texture
    xr_string detail_name;
    float detail_scale{1.f};
    bool detail_diffuse{true}, detail_bump{false};
    bool has_material{false};
    float material{0.f}; // what a .thm gives: the lighting pair's index plus the weight towards the next
};

struct Sound
{
    xr_string file;
    bool has_min{false}, has_max{false}, has_volume{false}, has_ai{false}, has_type{false};
    float min_distance{1.f}, max_distance{300.f}, volume{1.f}, ai_distance{300.f};
    xr_vector<xr_string> type; // the game's sound type words, ORed
    bool copyrighted{false};   // not played while the streamer mode is on
    xr_string streamer;        // the sound that plays in its place then, the game's name; empty for none
};

struct Surface
{
    xr_string texture, shader; // empty: the model's own
};

struct Bone
{
    xr_string name;
    xr_string shape; // none, box, sphere, cylinder; empty: the model's own
    bool has_mass{false};
    float mass{0.f};
    xr_string material;
};

struct Animation
{
    xr_string name;
    bool has_speed{false};
    float speed{1.f};
    u16 set{0}, clear{0}; // motion flags (esm*) to raise and to drop
};

struct Model
{
    xr_string file;
    xr_vector<Surface> surfaces; // by the model's order
    xr_vector<Bone> bones;
    xr_vector<Animation> animations;
    bool has_userdata{false};
    xr_string userdata;
};

// Finds and reads the descriptions of every enabled module; later modules win. Called once,
// when the modules are mounted.
XRCORE_API void Build();

// By the game's name: the path under textures / sounds / meshes without the extension (any
// case, either slash). Null when no module describes it.
XRCORE_API const Texture* FindTexture(pcstr name);
XRCORE_API const Sound* FindSound(pcstr name);
XRCORE_API const Model* FindModel(pcstr name);

// Keyed by the game's name, lower case with back slashes.
XRCORE_API const xr_map<xr_string, Texture>& Textures();
XRCORE_API const xr_map<xr_string, Sound>& Sounds();

// The surface of a visual a model pool loads by `visual_name`: a model's own name when it has
// no children (`whole`), "<model>:<n>" for its n-th child. Null when it is not described.
XRCORE_API const Surface* FindSurface(pcstr visual_name, bool whole, const Model** model = nullptr);
} // namespace XMS::Assets
