// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Procedural shrubs (docs/dead-air/SHRUBS.md). The level's stock bushes are placements: where one stands, a
// bush of a real ChEZ species grows instead - picked by the plant community of the spot and the stock model's
// class and size, built at level load from its species' branching rules (da_shrub_gen.h), with its own leaves,
// flowers, fruits and catkins following the species' phenology day by day, its axes bending under the wind
// service's field and motors one by one. Near: wood, leaves and blooms; the middle distance: the main wood and
// leaf clusters; far: crown blobs. The stock bush leaves the sector hierarchy while its replacement stands. The shrubs
// are a population of the vegetation core (da_veg.h: the hooks of every pass, da_shrubs.cpp: the population).
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// r__shrubs_stats (QA): the tiers, chunks and elements of the frame, the bushes per species.
void da_shrubs_stats();
// r__shrubs_species_pin <name|off> (QA): every bush grows that species.
void da_shrubs_pin_species(pcstr name);
// r__shrubs_probe (QA): the bushes nearest the camera - species, distance, size, template, tier.
void da_shrubs_probe(pcstr args);
} // namespace xray::render::RENDER_NAMESPACE
