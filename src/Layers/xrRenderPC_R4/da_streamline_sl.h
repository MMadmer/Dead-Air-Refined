// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The Streamline side the DLSS backends share (da_upscaler_dlss.cpp, da_framegen_dlss.cpp): the
// feature entry points of the loaded plugins, this frame's token, the common constants (set once
// per frame, DLSS and DLSS-G read the same), and resources in the form the running API needs.
#pragma once

#include "da_streamline.h"

#include <sl.h>
#include <sl_helpers.h>

namespace xray::render::RENDER_NAMESPACE
{
// Entry points of the features; null where the feature is not available on this device.
struct StreamlineFunctions
{
    PFun_slDLSSGetOptimalSettings* dlssGetOptimalSettings{};
    PFun_slDLSSSetOptions* dlssSetOptions{};
    PFun_slDLSSGSetOptions* dlssgSetOptions{};
    PFun_slDLSSGGetState* dlssgGetState{};
    PFun_slReflexSleep* reflexSleep{};
    PFun_slReflexSetOptions* reflexSetOptions{};
    PFun_slPCLSetMarker* pclSetMarker{};
    PFun_slPCLGetState* pclGetState{};
};
const StreamlineFunctions& da_sl();

// The one viewport of the game; DLSS-G expects its tags on id 0.
constexpr u32 da_sl_viewport_id = 0;

// This frame's token (taken at the frame start marker, or here if the frame loop sent none).
// Null while Streamline is off.
const sl::FrameToken* da_sl_frame();

// The camera of the frame as the common constants need it. Matrices are the engine's (row vectors,
// D3DX layout), without jitter; the camera basis comes from the inverse view.
struct StreamlineCamera
{
    Fmatrix view, project;         // this frame
    Fmatrix prevView, prevProject; // previous frame (the same as this one after a cut)
    float nearZ{}, farZ{};
    float fovY{};                  // radians, vertical
    u32 renderW{}, renderH{};
    float jitterX{}, jitterY{};    // render pixels, as the projection carried them
    bool reset{};
};
// Sets the common constants of this frame's token once; a second call in the same frame (DLSS,
// then DLSS-G) keeps the first. false = Streamline is off or the call failed.
bool da_sl_constants(const StreamlineCamera& camera);

// A texture as sl::Resource: the D3D11 resource with state 0 on the DX11 module, the native D3D12
// resource with its current state on the DX12 module.
sl::Resource da_sl_texture(ID3D11Resource* resource, u32 d3d12State);

// Logs a failed call once per call site and result.
void da_sl_report(pcstr what, sl::Result result);
} // namespace xray::render::RENDER_NAMESPACE
