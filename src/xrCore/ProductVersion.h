// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#define DAR_VERSION_MAJOR 1
#define DAR_VERSION_MINOR 6
#define DAR_VERSION_PATCH 4
#define DAR_VERSION_BUILD 0
#define DAR_VERSION_RESOURCE DAR_VERSION_MAJOR, DAR_VERSION_MINOR, DAR_VERSION_PATCH, DAR_VERSION_BUILD
#define DAR_VERSION_STRING "1.6.4"
#define DAR_WIDEN_IMPL(value) L##value
#define DAR_WIDEN(value) DAR_WIDEN_IMPL(value)
#define DAR_VERSION_WSTRING DAR_WIDEN(DAR_VERSION_STRING)
// What this engine can do, space-separated, in the version resource (`EngineAbilities`) of every
// program that implements it - xrEngine.exe, xrEngine.dll, xrCore.dll and xrGame.dll - for tools
// that start it; a word counts only when all of them state it (a mixed installation does not). A
// word is added, never changed or removed:
// - fsltx-in-place: started with `-fsltx` naming a config outside its folder (quoted when it holds
//   a space), the engine writes nothing into the installation and takes the archives from that
//   config's `$arch_dir$`;
// - long-paths: the executable is long-path aware and the file system takes a path below the
//   config's roots as long as its buffers (`string_path`, 519 characters) wherever Windows long
//   paths are enabled;
// - level-pieces: a module's <module>\levels\<level>\pieces\<name>\ is loaded beside the level, hung on its
//   sectors and streamed by distance (docs/dead-air/XMS_ARCHITECTURE.md 8.3);
// - level-cuts: a module's levels\<level>\overlay.cut clips the level's static collision and its own visuals
//   exactly (XMS_ARCHITECTURE.md 8.4);
// - ai-edits: a module's levels\<level>\overlay.aiedit moves and closes the level's AI nodes in place (8.4).
#define DAR_ENGINE_ABILITIES "fsltx-in-place long-paths level-pieces level-cuts ai-edits"

#ifndef RC_INVOKED
namespace DeadAirRefined
{
inline constexpr char Version[] = DAR_VERSION_STRING;
inline constexpr wchar_t VersionWide[] = DAR_VERSION_WSTRING;
inline constexpr char ProductName[] = "Dead Air: Refined";
}
#endif
