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
struct LuaValue;

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

// Takes away what earlier starts published and this one did not - the characters' files and the
// items' alike.
void FinishCharacters();

// What the game makes for its own loaders from a module's descriptions (the characters', the
// items'): written under $app_data_root$\xms_generated\<relative> when it changed and mounted at
// <relative> under $game_data$.
void PublishGenerated(const xr_string& relative, const xr_string& text);
// Text a module wrote - UTF-8 when a tool wrote it, Windows-1251 when a person did - as the game
// reads it, and as the text of an XML element.
xr_string GameText(const xr_string& text);
xr_string XmlText(const xr_string& text);
// A text of one string or { language = text } in a language, falling back to rus; the languages it names.
xr_string TextIn(const LuaValue& text, pcstr lang);
void LanguagesOf(const LuaValue& text, xr_vector<xr_string>& out);
}
