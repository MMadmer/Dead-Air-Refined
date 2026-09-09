#pragma once

// Particle effects the game does not play, by name. Mechanism here, names in data: the water
// rework carries its own ring physics (the ripple field), so the flat sprite rings the stock
// hit_fx set lays on the water - a white ring texture growing to metres, the distortion discs -
// are listed in dead_air_x64_water.ltx and skipped wherever they would play: as a child of a
// material pair's group, as a group child's own child, or played by name from the game. The
// sprays around them are not in the list and are untouched.

// Comma-separated effect names; nullptr or empty clears the list.
ENGINE_API void da_particle_suppress_set(pcstr names);
ENGINE_API bool da_particle_suppressed(pcstr name);
