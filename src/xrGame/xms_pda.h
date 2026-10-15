// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS PDA layouts - a module's own PDA interface.
//
// A module names a .pdaasset in mod.ltx ([pda] layout = <path in the module>). When it
// applies to the running game, the PDA keeps its device and its live map and shows the
// module's screens instead of the stock tabs: CUIPdaLayout builds them from the flat
// item list xms_pda.script produces (the reader itself, xms_pda_layout.script, is shared
// with the editor's preview). Nothing of the game is written: without a layout the PDA
// is the stock one, byte for byte, and the stock sections stay reachable from a layout
// (stock:<section>).
//
// {placeholders} a script sets through xms.pda.set ride the save (engine blob 0xFF03);
// pages, visibility and list rows are the interface's own state and start over with
// every PDA the level builds.
// Contract: docs/dead-air/MODDING.md, "PDA layouts".

#include "xrCore/XMS/xms_core.h"

class CUIPdaLayout;
struct lua_State;

namespace XmsPda
{
// The module whose layout the PDA uses: the last applying module in the load order that
// declares one. Null = the stock PDA. A second applying module with a layout is a
// conflict, reported once per session.
const XMS::Module* Owner();

// The layout of the PDA that is alive; set by CUIPdaLayout for its lifetime.
void SetLive(CUIPdaLayout* layout);
CUIPdaLayout* Live();

// {placeholder} values set at run time (cp1251). Every change is staged for the next save.
void SetVar(pcstr name, pcstr value);
bool GetVar(pcstr name, xr_string& value);
// The values the save (or this session) holds, for a PDA that is being built.
void Vars(xr_vector<std::pair<xr_string, xr_string>>& out);

void ScriptRegister(lua_State* L);
}
