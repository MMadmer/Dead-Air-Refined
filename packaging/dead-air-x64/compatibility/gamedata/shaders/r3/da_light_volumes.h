// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The local lamps drawn as instanced volumes (da_light_volumes.cpp, docs/dead-air/LIGHT_VOLUMES.md): the frame's lamps
// as records in one buffer, and per shadow atlas batch one instanced draw of the spots' cones, one of the point lamps'
// cube faces and one of the unshadowed points' spheres (da_light_volume.vs / .ps). It takes the place of the stock
// accumulation: two stencil draws and a lighting draw for every lamp, each with a pipeline change.

#ifndef DA_LIGHT_VOLUMES_H
#define DA_LIGHT_VOLUMES_H

#include "da_raw_buffer.h"

#define DA_LV_STRIDE		256		// bytes of a record: 16 float4

// The record (DaLvLight in da_light_volumes.cpp), float4 by float4:
//  0 pos     view-space position, 1 / (0.95 range)^2
//  1 color   colour (times the lamp's LOD), its specular share
//  2 dir     view-space direction, cos of the half cone (spot)
//  3 info    type | cube face << 4, flags, -, projector slot
//  4-7       view -> shadow map, rows
//  8-9       view -> projector xy, rows
//  10-12     the volume's world transform, rows (light::m_xform)
#define DA_LV_POINT			0u		// a sphere
#define DA_LV_SPOT			1u		// a cone
#define DA_LV_FACE			2u		// one cube face of a point lamp

#define DA_LV_SHADOW		1u		// samples the shadow atlas
#define DA_LV_LMAP_XFORM	2u		// the projector through its own rows
#define DA_LV_LMAP_SHADOW	4u		// the projector at the shadow coordinates (the whole atlas, or an unshadowed spot's own projection)
#define DA_LV_ENABLED		8u		// lights (a shadowed lamp whose map had no casters lights nothing)

#define DA_LV_XFORM_OFFSET	160u

#endif // DA_LIGHT_VOLUMES_H
