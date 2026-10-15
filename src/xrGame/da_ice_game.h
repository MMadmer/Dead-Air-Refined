// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#pragma once

// The ice under the actor and under fire (docs/dead-air/WEATHER.md, ice). The engine says where the ice holds
// (xrEngine da_ice.h) and the physics stands on it; here the weight cracks the weak ice and breaks it, a man in the
// water breaks a channel through it, a bullet chips it or holes it, a blast breaks it - with their sounds and
// particles.

class CActor;

// Once a frame for the actor (CActor::UpdateCL).
void da_ice_actor_update(CActor& actor);

// The static world was hit at `p` on a liquid triangle: the bullet's material there (the ice's where it holds), and its
// mark when it struck. `solid` = the material the bullet stopped on is the ice.
u16 da_ice_bullet_material(u16 material, const Fvector& p);
void da_ice_bullet_hit(const Fvector& p, bool solid);

// A blast at the water's surface point `p` over ice: breaks it by the blast's radius and the ice's strength. True when
// the ice was there (the caller's open-water response stays off).
bool da_ice_blast(const Fvector& p, float blast_radius);

// QA console commands (console_commands.cpp): qa_ice_goto, qa_ice_probe, qa_ice_bullet.
void da_ice_register_console();
