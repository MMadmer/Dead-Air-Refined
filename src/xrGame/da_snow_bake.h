// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The level's snow fields at 1 m (docs/dead-air/SNOW.md): ground and top heights, class weights (open,
// under trees, road, drift), accumulation and melt factors and the per-level season LUT, handed to xrEngine
// (da_snow_field_set). Built once per level load from the collision mesh.
#pragma once

void da_snow_bake_level();
