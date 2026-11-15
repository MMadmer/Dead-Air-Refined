// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// XMS level edits: the exact cuts of a level's static geometry (`levels\<level>\overlay.cut`) and
// the changes of its AI nodes in place (`levels\<level>\overlay.aiedit`), read physically from
// every applying module in module order, like overlay.xcform. Contract:
// docs/dead-air/XMS_ARCHITECTURE.md 8.4.

#include "xrCore/xrCore.h"
#include "xms_cut_clip.h"

namespace XMS
{
struct CutRegion
{
    u8 kind{}; // 0 everything, 1 ground
    Fvector centre{}, half{};
    float yaw{};
    float tolerance{};
    u32 columns{}, rows{};
    xr_vector<float> heights;

    // The clip's view of it; valid while the region lives.
    Cut::Region view() const
    {
        return {kind, {centre.x, centre.y, centre.z}, {half.x, half.y, half.z}, yaw, tolerance, columns, rows,
            heights.empty() ? nullptr : heights.data()};
    }
};

struct LevelCuts
{
    xr_vector<CutRegion> regions;
    u32 hash{}; // over every file read, in module order; 0 when there is none
};

struct AiEdit
{
    float x, z;
    float old_y, new_y;
    Fvector normal;
    u32 flags; // 1: closed
};

// `level` is the level's folder name. Hostile files are refused with one log line each.
XRCORE_API void ReadLevelCuts(pcstr level, LevelCuts& out);
XRCORE_API void ReadAiEdits(pcstr level, xr_vector<AiEdit>& out);

// The collision cuts first, the picture later and in another library: what the collision cut is
// kept here until the renderer writes the level's one line.
XRCORE_API void NoteCollisionCuts(pcstr level, u32 triangles);
XRCORE_API u32 TakeCollisionCuts(pcstr level);
} // namespace XMS
