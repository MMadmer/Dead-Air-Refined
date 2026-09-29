// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// Variable rate shading, tier 2 (docs/dead-air/dx12-features/vrs.md 3.6): one rate image per frame
// from depth, motion and luminance, applied per pass under a cap. Both modules compile it; without
// DirectX 12 everything answers "off".
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class CBackend;
class CRenderTarget;

enum class VrsPass : u8
{
    SunLight,
    SunVolumetric,
    Lights,
    LightsVolumetric,
    PuddleRefl,
    Forward,
    VolumetricCombine,
    SunshaftsBlur,
    Count
};

// R5, tier 2, shaders present, targets created (r__caps vrs=).
bool da_vrs_supported();
// The mode the quality preset asks for: 0 off, 1 conservative, 2 aggressive ([preset] vrs=).
int da_vrs_preset_mode();
// Before the sun accumulation: the rate image of this frame.
void da_vrs_rate_pass(CBackend& cmd_list);
// Before the upscale: the luminance contrast the next frame's rate image reads, from the render-size
// frame; while an upscaler runs, after the upscale from its resolved output instead.
void da_vrs_luma_pass(CBackend& cmd_list);
void da_vrs_luma_upscaled_pass(CBackend& cmd_list);
// After phase_pp: the r__vrs_debug overlay.
void da_vrs_debug_pass(CBackend& cmd_list);
// Back to 1x1 without an image.
void da_vrs_reset(CBackend& cmd_list);
// With the render targets (after the velocity target) and before they go.
void da_vrs_targets_create(CRenderTarget& target);
void da_vrs_targets_destroy(CRenderTarget& target);

// The pass's rate while in scope, 1x1 without an image after it.
struct da_vrs_scope
{
    da_vrs_scope(CBackend& cmd_list, VrsPass pass);
    ~da_vrs_scope();
    da_vrs_scope(const da_vrs_scope&) = delete;
    da_vrs_scope& operator=(const da_vrs_scope&) = delete;

private:
    CBackend& m_cmd_list;
    bool m_active{};
};

// QA (r__vrs_passes): bit n lets VrsPass n shade at a reduced rate; every pass by default.
extern int ps_r__vrs_passes;
// QA (r__vrs_edge): the scale of the depth ratio a silhouette needs (k_jump); 1 by default.
extern float ps_r__vrs_edge;

// Between da_vrs.cpp and r4_rendertarget_phase_vrs.cpp: the mode asked for (r__vrs, else the
// preset), and this frame's mode (0 off, 1 conservative, 2 aggressive; r__vrs_force runs as 2) and
// rate image (null: the image passes shade 1x1).
int da_vrs_requested_mode();
int da_vrs_frame_mode();
ID3DTexture2D* da_vrs_frame_image();
} // namespace xray::render::RENDER_NAMESPACE
