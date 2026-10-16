// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Frame generation (DirectX 12 and Vulkan): AMD FSR frame generation and NVIDIA DLSS frame generation
// behind one interface, chosen by r__framegen. See docs/dead-air/DX12_RENDERER.md and VULKAN_RENDERER.md.
#pragma once

#if defined(XR_D3D12)
#include "Layers/xrRenderDX12/d12_api.h"
#elif defined(XR_VULKAN)
#include "Layers/xrRenderVK/vk_api.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
// What a backend gets once per frame, after the 3D scene and before the UI.
struct FrameGenFrame
{
    ID3D11Resource* depth = nullptr;    // scene depth, render size (R24G8)
    ID3D11Resource* velocity = nullptr; // render size, R16G16F, NDC(cur) - NDC(prev), unjittered, y up
    ID3D11Resource* hudless = nullptr;  // display size, RGBA8: the finished frame without UI
    u32 renderW = 0, renderH = 0;
    u32 displayW = 0, displayH = 0;
    float jitterX = 0, jitterY = 0;     // render pixels
    float frameTimeMs = 16.6f;
    float nearZ = 0.05f, farZ = 1000.f;
    float fovY = 1.f;                   // radians, vertical (deg2rad(Device.fFOV))
    bool reset = false;
    Fmatrix view, project;              // unjittered, this frame
    Fmatrix prevView, prevProject;      // unjittered, previous frame
    Fvector cameraPosition, cameraUp, cameraRight, cameraForward;
    u64 frameIndex = 0;
};

class IFrameGen
{
public:
    virtual ~IFrameGen() = default;
    virtual pcstr Name() const = 0;
    // The swap chain this backend presents through (null = a plain one). It must stay valid until the
    // module unloads: the swap chain keeps presenting through it after the backend is deleted
    // (renderer destroy) and destroys it only with the device - a static object.
#if defined(XR_D3D12)
    virtual xrd3d12::SwapChainProvider* Provider() = 0;
#elif defined(XR_VULKAN)
    virtual xrvk::SwapChainProvider* Provider() = 0;
#endif
    // generated = frames generated per rendered frame (1 = 2x, 2 = 3x, 3 = 4x). Called after the
    // swap chain runs through Provider(), and again after Shutdown on every renderer reset (a new
    // display size or `generated`).
    virtual bool Init(u32 displayW, u32 displayH, u32 generated) = 0;
    virtual void Shutdown() = 0;
    // Render thread, immediate context, once per frame after the scene and the hudless copy, and
    // only while active (SetActive(true) comes first in the same frame).
    virtual void Prepare(const FrameGenFrame& frame) = 0;
    // Off while a menu, the loading screen or a pause is up (generated frames would smear the UI).
    // Off takes effect at once, before the frame's Present: no Prepare may follow.
    virtual void SetActive(bool active) = 0;
};

struct FrameGenBackend
{
    int token = 0;             // r__framegen token id it serves (st_fg_fsr = 1, st_fg_dlss = 2..4)
    pcstr name = nullptr;
    IFrameGen* (*create)() = nullptr;
    bool (*supported)() = nullptr; // on this adapter/driver/OS right now
    u32 (*maxGenerated)() = nullptr; // highest `generated` it supports (may be null = 1)
};

void da_framegen_register(const FrameGenBackend& backend);
struct da_framegen_registrar
{
    explicit da_framegen_registrar(const FrameGenBackend& backend) { da_framegen_register(backend); }
};

// ---- the manager (da_framegen.cpp; the DirectX 11 module answers "unavailable") -----------------

// Renderer reset_begin, before any surface is released: the backend stops and the swap chain
// finishes the presents it queued (they read the hudless copy).
void da_framegen_stop();
// Renderer create (before the main target) and reset_begin (after the targets are gone), where no
// back buffer view exists: the running backend stops, and when r__framegen asks for another one
// the swap chain is rebuilt through the new backend's provider.
void da_framegen_switch();
// Renderer create and reset_end, once the targets exist: the backend starts at the display size.
void da_framegen_start();
// Renderer destroy.
void da_framegen_destroy();
// End of CRender::Render(): the back buffer holds the finished 3D frame and no UI yet.
void da_framegen_frame();
// Right before Present (D3DXRenderBase::End): a frame that was not prepared generates nothing.
void da_framegen_present();
// A backend is initialised (generating or not): the motion vectors are rendered for it.
bool da_framegen_running();
} // namespace xray::render::RENDER_NAMESPACE
