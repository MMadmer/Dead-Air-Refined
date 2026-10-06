// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Season constants bound by the standard binders (docs/dead-air/SEASONS.md).
#ifndef DA_SEASON_COMMON_H
#define DA_SEASON_COMMON_H

uniform float4 da_season_day; // x D/365, y D_prev/365, z 1 while a QA command drives D, w hour of the day (0..24)
uniform float4 da_season_env; // x hoarfrost now, y dew now, z rain wetness, w snowfall now (0..1)

#endif // DA_SEASON_COMMON_H
