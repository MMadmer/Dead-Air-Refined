#pragma once

// XMS config composition: per-module ltx overlays (gamedata\configs\xms\*.ltx)
// and directive patches (patch\*.ltxp) applied to a fully built CInifile.

#include "xrCore/xrCore.h"

class XRCORE_API CInifile;

namespace XMS
{
// Applies every enabled module's config overlays and .ltxp patches to the
// given ini, in module load order. Called once right after InitSettings
// builds pSettings. No-op when XMS is inactive.
XRCORE_API void ApplyConfigStage(CInifile* ini);
}
