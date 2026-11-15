// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// The exact cut of a level's static triangles by a region of overlay.cut
// (docs/dead-air/XMS_ARCHITECTURE.md 8.4). Pure and free of the engine's types on purpose: the
// collision and the visuals call the same function, the authoring tool implements the same
// algorithm for its preview, and tools/qa/cuts checks it without the game.

#ifdef XMS_CUT_STANDALONE
#define XMS_CUT_API
#else
#include "xrCore/xrCore.h"
#define XMS_CUT_API XRCORE_API
#endif

namespace XMS::Cut
{
struct Region
{
    int kind; // 0: everything inside the box, 1: the ground within `tolerance` of `heights`
    float centre[3];
    float half[3];
    float yaw; // radians about +Y
    float tolerance;
    unsigned columns, rows; // kind 1: the grid, two by two at least
    const float* heights; // kind 1: columns * rows, row by row from -z, each row from -x
};

constexpr int max_points = 32;
constexpr int max_triangles = 16;

// What replaces a triangle: points as weights of its three vertices (a vertex that stays is the
// exact unit weight; every attribute of a point is that weighted sum of the vertices'), and
// triangles over the points with the triangle's winding.
struct Result
{
    int points;
    float weights[max_points][3];
    int triangles;
    unsigned char indices[max_triangles][3];
};

// False: the triangle is kept whole and unchanged. True: it is replaced by `out`, which may hold
// no triangle at all.
XMS_CUT_API bool ClipTriangle(const Region& region, const float a[3], const float b[3], const float c[3], Result& out);
} // namespace XMS::Cut
