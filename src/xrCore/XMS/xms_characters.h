// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS characters - a module's own NPCs. One .npcasset per character, anywhere in the module, is
// all a module ships: the game reads them when the configs come up and makes what its loaders
// want - the specific character and its profile, the strings, the spawn and squad sections, a
// trader's config. Contract: docs/dead-air/MODDING.md, "A module's characters".

#include "xrCore/xrCore.h"

class CInifile;

namespace XMS
{
struct Module;

// What a module's characters add to system.ltx. The files they need are already published.
struct CharacterConfigs
{
    xr_string sections;       // the spawn and squad sections, ltx text
    xr_string profiles;       // goes on [profiles] files
    xr_string characters;     // goes on [profiles] specific_characters_files
    u32 count{0};
};

// False when the module has no character the game can use.
bool BuildCharacters(const Module& m, const CInifile& settings, CharacterConfigs& out);

// Takes away what earlier starts published and this one did not.
void FinishCharacters();
}
