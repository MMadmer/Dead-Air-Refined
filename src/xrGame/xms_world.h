// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS world rules - what of the original game a mode keeps, where the player may go,
// which transitions are closed, and where a new game starts.
//
// Resolved ONCE per new game from the [world] of every applying module, with the
// player's choices from the new-game screen where an author offered them, and kept in
// the save's .scov (engine-owned blob 0x584DFF02): a save plays in the world it was
// started with, whatever the modules say later. Read by the engine - the spawn
// composer, level changers, scripted level changes - and by Lua through xms.world.
// Contract: docs/dead-air/MODDING.md, "World rules".

#include "xrCore/XMS/xms_core.h"

class CInifile;
class CLevelChanger;
struct lua_State;

namespace XmsWorld
{
// ---- the session --------------------------------------------------------------
// A new game: the rules of every applying module, the player's new-game choices
// ([character_creation] new_game_xmsw_<rule> = 1|0) where the author offered them.
// Runs after the mode set is published and before the spawn composer. `options` may
// be null (no options file) - the authors' defaults stand then.
void ResolveForNewGame(const CInifile* options);
// A loaded game: the world the save was started with; null = the save has none.
void OnLoaded(const xr_vector<u8>* blob);

// true when any rule, area or lock is in force this session
bool Declared();

// A population or quest rule (XMS::WorldRules::Rule) on a level: true = the original
// game's content stays. A null level matches only rules that apply everywhere.
bool Keep(u32 rule, pcstr level);
// A quest's runtime override of a rule for the whole world: 1 keep, 0 remove, -1 clear.
void SetOverride(u32 rule, int value);

// ---- area and transitions -------------------------------------------------------
// The playable area: the levels a scripted jump may lead to. Everywhere when no module
// declared one.
bool InArea(pcstr level);
void SetAreaLevel(pcstr level, bool in_area);
// Whether a level changer lets the player through: an explicit open beats every lock,
// then an explicit lock, a locked level, and a destination outside the area.
bool TransitionOpen(pcstr changer, pcstr level, pcstr dest_level);
void SetTransitionLock(pcstr changer, bool locked);
void SetLevelLock(pcstr level, bool locked);
// The changer's enabled flag and invitation from the state; untouched when the world
// says nothing about transitions.
void Apply(CLevelChanger& changer);
// Re-applies the state to every level changer that is online.
void RefreshOnline();
// Every level change reaches the server here (CALifeUpdateManager::change_level): a
// destination outside the area is refused - unless a changer a quest opened explicitly
// leads there, or the developer's console jump asked for it (AllowNextLevelChange).
bool LevelChangeAllowed(u16 dest_game_vertex_id);
void AllowNextLevelChange();

// ---- the player start -----------------------------------------------------------
// The composer placed the actor on a module's Player Start.
void NoteStartPlaced(pcstr module_id, pcstr level);
// The first scripted jump after a new game whose start a module placed is the faction
// start of the new-game screen, which would carry the player away from it. True: drop it.
bool ConsumeFactionJump();
// Lua, a few seconds into the new game: no faction jump is coming any more.
void SettleStart();

// ---- module waypoint paths ------------------------------------------------------
// Registers the [path:*] sections the applying modules' spawn layers carry for this
// level as runtime patrol paths, once the level's navmesh is loaded.
void RegisterModulePaths(pcstr level_name);

// ---- lua and console --------------------------------------------------------------
void ScriptRegister(lua_State* L);
void Describe(xr_string& out);
}
