#pragma once

// Mods can opt the installation out of the online services this build ships with: the
// automatic update check and the bug report menu entry. Both are pointless - and the
// report is actively misleading - once the game is not the one this project published.
//
// A mod declares itself by name, and the names are shown to the player, so a broken
// installation can be traced back to whoever changed it.
namespace ModOptOut
{
// Names in declaration order, without duplicates. Empty when nothing opted out.
const xr_vector<shared_str>& DisablingMods();

// True when at least one mod opted out. Cheap - use freely per frame.
bool AutoUpdateDisabled();

// Declared from Lua (dead_air_disable_auto_update) or from an LTX section. The name is
// what the player sees; empty or blank names are refused so the list stays meaningful.
void DisableAutoUpdate(pcstr modName);

// Reads the declarations that live in configs. Called once when the system.ltx-backed
// settings are available. Repeated calls are harmless - names are deduplicated.
void LoadFromConfig();
} // namespace ModOptOut
