// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Every creature, corpse and settled object stamps the snow (docs/dead-air/SNOW.md): feet with a contact
// gate, body capsules, one-off stamps of objects coming to rest, explosion craters; xrEngine's
// da_snow_stamp_add() collects them for the renderer's trail maps and the grass push map.
#pragma once

class CEntityAlive;

// Per frame for a living or dead creature (called from its update).
void da_snow_deform_entity(CEntityAlive& entity);
// An explosion at `pos` with blast radius `radius`.
void da_snow_deform_crater(const Fvector& pos, float radius);
// A bullet (a pellet) striking the static world at `point`, flying along `dir`; `wallmark` its mark's size (m).
void da_snow_deform_bullet(const Fvector& point, const Fvector& dir, float wallmark);

class CPhysicsShellHolder;
// A physics object that just came to rest - a corpse, loot, an artefact, a prop: its footprint once.
void da_snow_deform_settled(CPhysicsShellHolder& object);
// Once per frame from the view entity's update: the physics in motion ploughs (a barrel rolling, a crate pushed,
// a body dragged or kicked), the campfires melt the snow round them, objects lying at rest that enter the stamp
// window (or were there when the snow came) get their footprint, and the memory of the creatures is pruned.
void da_snow_deform_frame();
// QA (qa_snow_probe): what the snow's gates see at a point - the tops round it, its cover, the depths.
void da_snow_probe(const Fvector& p);
// A new level (the bake): every remembered creature and object is forgotten.
void da_snow_deform_reset();
