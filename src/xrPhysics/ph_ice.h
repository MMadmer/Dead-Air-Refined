// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// The season's ice on a liquid triangle (docs/dead-air/WEATHER.md, ice; xrEngine da_ice.h). The water mesh is
// passable: what touches it from above sinks to the bed. Where the ice holds, a contact on that triangle is a contact
// on the ice material instead, and everything resting on the surface stands on it. Only a body lying on the surface
// takes it - one already standing through it (a man wading in a hole, a body on the bed) keeps passing.

// The material of a contact at pos with a triangle of material `material`, the body that far under its plane.
u16 ph_contact_material(u16 material, const float* pos, float depth);
