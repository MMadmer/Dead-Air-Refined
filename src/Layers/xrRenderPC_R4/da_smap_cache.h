// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

class light;

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;

// The static casters of a still local light kept in a depth atlas between frames, after DOOM Eternal's cached shadow
// slices: a face draws its walls, floors and props once, then each frame copies them into the live map and draws
// only what moves - characters, physics objects, the swaying trees, grass and shrubs.
enum class DaSmapCache : u8
{
    off, // the stock face: every caster, every frame
    fill, // the statics go into the light's slot first, the rest into the live map after the copy
    use, // the build skips the statics, the face starts from the slot's copy
};

// The face size from the ladder 32, 48, 64 ... 1536 with a dead band (r__light_shadow_cache): a still light keeps
// one size, so its slot stays valid while the camera walks.
u32 da_smap_cache_size(u32 raw, u32 previous);
// The light's mode for this frame (render_lights, after the atlas packing, before its caster build).
DaSmapCache da_smap_cache_mode(light* L);
// flush_lights, fill: the slot as the depth target, cleared; the statics are drawn next.
void da_smap_cache_fill_begin(CBackend& cmd_list, light* L);
// The slot's depth into the light's rect of the live map (its viewport set by phase_smap_spot).
void da_smap_cache_copy(CBackend& cmd_list, light* L);
void da_smap_cache_release(light* L);
// A level load or a device reset: every slot forgotten.
void da_smap_cache_reset();
} // namespace xray::render::RENDER_NAMESPACE
