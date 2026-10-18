// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
class light;

// The local lamps drawn as instanced volumes (docs/dead-air/LIGHT_VOLUMES.md, shaders r3\da_light_volumes.h). The stock
// accumulation drew every lamp's volume three times - two stencil passes and the lighting - each with its own pipeline:
// three hundred draws for a hundred lamps, the cost a Vulkan or DirectX 12 driver charges most for. Here an atlas
// batch's lamps are records in one buffer and its spots, point lamp faces and points are three instanced draws of the
// stock volume meshes, each lamp shaded by the one shader exactly as the stock ones did. A pixel pays for the lamps
// whose volumes cover it and nothing else.

// The batch's shadowed lamps whose maps were drawn (the others had no casters and light nothing, as before) and, with
// the last batch or alone, the visible unshadowed lamps: their lighting into the accumulator (bound by
// phase_accumulator). The spots among them have their shadow transforms (compute_xf_spot).
void da_lv_draw(CBackend& cmd_list, const xr_vector<light*>& shadowed, const xr_vector<light*>* unshadowed);
// A level load, a new render target: the buffer and the shaders go, made again at the next draw.
void da_lv_reset();
} // namespace xray::render::RENDER_NAMESPACE
