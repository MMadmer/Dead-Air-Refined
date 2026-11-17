// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS config composition: per-module items (*.itemasset), traders, loot tables and recipes
// (*.tradeasset, *.lootasset, *.recipeasset), ltx overlays (gamedata\configs\xms\*.ltx) and directive
// patches (patch\*.ltxp) applied to a fully built CInifile; what they patch of other configs (`#file`)
// is kept and applied to every ini that reads those files.

#include "xrCore/xrCore.h"
#include "xms_ltxp.h"

class XRCORE_API CInifile;

namespace XMS
{
// Applies every enabled module's items, tables, config overlays and .ltxp patches to the
// given ini, in module load order. Called once right after InitSettings
// builds pSettings. No-op when XMS is inactive.
XRCORE_API void ApplyConfigStage(CInifile* ini);

// ---- patches of other configs (`#file`) --------------------------------------
// Read-only once the stage has run: any thread may ask.

// Whether a reader of a config should note the files it reads: while modules may patch them -
// until the stage has run, and after it while any patch of another config was kept.
bool WantLtxFiles();
bool HasFilePatches();
// The path of a file under $game_config$ as patches name it (ConfigPath); false for a file outside it.
bool ConfigRelative(pcstr path, xr_string& relative);
// Applies to `ini` what the modules patch of `files` (sorted, unique), modules in load order,
// with the inheritance of every other change. True when any of it was for this ini.
bool ApplyFilePatches(CInifile& ini, const xr_vector<xr_string>& files);
// While held on this thread, a config read from files is read as written: what the game is about
// to write back must not carry a module's patch into the player's file.
XRCORE_API void HoldFilePatches(bool hold);

struct DirectiveCounts
{
    u32 ops{0};
    u32 skipped{0};
};
// Applies directives in their order at a module's layer; a key is applied only under its own
// header, as a patch file reads. `ledger`: the rows a patch of the system config leaves.
void ApplyDirectives(CInifile& ini, const LtxDirective* const* list, size_t count, pcstr module, u16 layer,
    bool ledger, DirectiveCounts& counts);
} // namespace XMS
