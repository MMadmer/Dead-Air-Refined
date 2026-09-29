// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// NVIDIA Streamline 2.14.1 (docs/dead-air/DX12_RENDERER.md, 1): the core both renderer modules
// share - loading the signed interposer, slInit in manual-hooking mode, the device and swap chain
// hand-over, one frame token per frame, the Reflex/PCL latency markers, and what the adapter runs
// (DLSS, DLSS frame generation, Reflex). Everything is optional: a missing or unsigned DLL, a
// non-NVIDIA adapter or a failed slInit leave Streamline off and the game runs without it.
// The SDK-typed part for the DLSS backends is da_streamline_sl.h.
#pragma once

#include "xrEngine/Render.h"

#ifdef XR_D3D12
#include "Layers/xrRenderDX12/d12_api.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
// ---- device lifetime (dx11HW.cpp, global CHW only) ---------------------------------------------

// Verifies and loads sl.interposer.dll next to the executable and runs slInit for this adapter.
// Before the device exists. -no_streamline skips it.
void da_streamline_init(IDXGIAdapter1* adapter);
#ifdef XR_D3D12
// xrd3d12::DeviceCreateInfo hooks: slSetD3DDevice on the native device, and the direct queue made
// through Streamline's device proxy when DLSS frame generation can run (it watches that queue).
void da_streamline_d3d12_device_created(ID3D12Device* device);
HRESULT da_streamline_d3d12_create_queue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc,
    ID3D12CommandQueue** queue);
// The swap chain to present through while no frame generation backend owns it: the factory is
// upgraded, so sl.common sees every present (its per-frame bookkeeping). Null without Streamline.
xrd3d12::SwapChainProvider* da_streamline_swapchain_provider();
// The factory another backend's swap chain is made through, so sl.common still sees every present
// (FSR frame generation with DLSS upscaling): Streamline's proxy, referenced, or null without
// Streamline. DLSS frame generation is unloaded first: it would take any chain the proxy makes.
IDXGIFactory2* da_streamline_factory(IDXGIFactory2* factory);
#else
void da_streamline_d3d11_device_created(ID3D11Device* device);
// Replaces the swap chain by Streamline's proxy in place (sl.common hooks its Present).
void da_streamline_d3d11_swapchain_created(IDXGISwapChain*& swapChain);
#endif
// slShutdown: before the swap chain and the device are released.
void da_streamline_shutdown();
// After they are released (the D3D12 device proxy goes last).
void da_streamline_device_released();
// The window procedure of the game window: PC latency pings arrive as a window message.
void da_streamline_window_message(UINT message);

// ---- the frame loop ------------------------------------------------------------------------

void da_streamline_frame_marker(IRender::FrameMarker marker);
// true = Reflex applies the frame limit (frame generation runs) and the engine's limiter must not.
bool da_streamline_frame_limit(u32 fpsLimit);

// ---- what the adapter runs (after the device exists) ----------------------------------------

bool da_streamline_active();   // slSetD3DDevice succeeded
bool da_streamline_dlss();     // DLSS Super Resolution / DLAA
bool da_streamline_dlssg();    // DLSS frame generation (DX12 module only)
u32 da_streamline_dlssg_max(); // frames it can generate per rendered frame (1 = 2x, 3 = 4x), 0 = none
bool da_streamline_hags();     // hardware-accelerated GPU scheduling is on for the adapter

// ---- frame generation (da_framegen_dlss.cpp) --------------------------------------------------

// Reflex low-latency mode and its frame limiter: on while DLSS frame generation runs.
void da_streamline_low_latency(bool on);
#ifdef XR_D3D12
// Loads or unloads sl.dlss_g (it attaches to every swap chain made through an upgraded factory).
// Unloading waits until no DLSS-G swap chain is alive; returns whether the state is as asked.
bool da_streamline_dlssg_load(bool loaded);
// Bookkeeping of the DLSS-G swap chain (its provider keeps it).
void da_streamline_dlssg_chain(bool alive);
// The swap chain provider of DLSS frame generation: loads sl.dlss_g and makes the chain through
// the upgraded factory. A static object (the swap chain may outlive the backend).
xrd3d12::SwapChainProvider* da_streamline_dlssg_provider();
#endif
} // namespace xray::render::RENDER_NAMESPACE
