// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// The depth pyramid of the G-buffer stage (r3\da_hzb.cs, r3\da_hzb.h; docs/dead-air/GRASS.md, "Occlusion"): built between
// the draws from the depth drawn so far, read by the compute passes that cull against it - the trees' cut after the
// opaque world and again after the near trees, the blades' generation after every opaque surface but them.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;

// The pyramid of the depth as it stands (immediate context, in the G-buffer stage): false without one (MSAA keeps no
// depth copy, or the texture failed), and the culling passes then cull nothing by it.
bool da_hzb_build(CBackend& cmd_list);
// The pyramid's constants for a compute pass (r3\da_hzb.h: da_hzb, da_hzb_lv, da_hzb_view, da_hzb_proj, da_hzb_proj2) and
// its texture in compute slot `slot`; `on` false sets the test off. Within the pass's dispatch, after its constants.
void da_hzb_bind(CBackend& cmd_list, u32 slot, bool on);
// The pass's slot free again, after its dispatch.
void da_hzb_unbind(CBackend& cmd_list, u32 slot);
// The frame's last pyramid kept for the next frame, with the camera it was built under (the end of the G-buffer stage):
// the cluster hierarchy's first pass tests against it (da_clusters.cpp, the two passes of the main view).
void da_hzb_keep_previous(CBackend& cmd_list);
// The last frame's pyramid: whether there is one of this size, and bound like da_hzb_bind with its own camera.
bool da_hzb_bind_previous_ready();
void da_hzb_bind_previous(CBackend& cmd_list, u32 slot);
// The texture and the pass, at the renderer's shutdown.
void da_hzb_destroy();
} // namespace xray::render::RENDER_NAMESPACE
