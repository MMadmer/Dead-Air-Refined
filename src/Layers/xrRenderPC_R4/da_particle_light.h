// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class light_Package;
class CResourceManager;

// The froxel volume's frame (r4_rendertarget_phase_froxels.cpp), for the passes that read it outside it: the lit
// particles take the sun's visibility from it.
struct DaFroxelFrame
{
    u32 frame = 0; // the frame the volume was injected on
    Fvector4 grid{}; // da_froxel_grid, da_froxel_range, da_froxel_ray (shaders/r3/da_froxel.h)
    Fvector4 range{};
    Fvector4 ray{};
};
const DaFroxelFrame& da_froxel_frame();

// The lit particles (shaders/r3/particle.vs): the frame's nearest local lights, taken as the lights are rendered,
// and the constants the particle vertex shader lights itself with.
void da_particle_light_snapshot(const light_Package& lights);
// Lit particles were drawn lately: the froxel volume is kept for their sun even without its glow.
bool da_particle_light_wanted();
void da_particle_light_register(CResourceManager& resources);
} // namespace xray::render::RENDER_NAMESPACE
