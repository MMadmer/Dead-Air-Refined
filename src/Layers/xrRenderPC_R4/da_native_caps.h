// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The DirectX 12 capabilities beyond the D3D11 feature set (docs/dead-air/dx12-features/infra.md 3.1):
// CHW::native after the device exists, the command-line switches that mask them, and the keys they
// give r__caps (PLAN.md 2.6). Both modules compile it; the R4 module knows only what the D3D12 probe
// cached about the adapter.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// A capability switched off for the run from the command line (fallback tests, troubleshooting).
enum DaNativeDisable : u32
{
    DaNativeDisableRT = 1u << 0,    // -dx12_no_rt
    DaNativeDisableVRS = 1u << 1,   // -dx12_no_vrs
    DaNativeDisableMS = 1u << 2,    // -dx12_no_ms
    DaNativeDisableAsync = 1u << 3, // -dx12_no_async
    DaNativeDisableSM6 = 1u << 4,   // -dx12_no_sm6
};
u32 da_native_disabled_features();

// Fills HW.native once the device exists (CHW::CreateDevice, both modules).
void da_native_caps_on_device(ID3D11Device* device);

// R4: the adapter's tiers from the D3D12 probe's cache (render-caps-d3d12.cache). The reader lives
// with the probe (r2_test_hw.cpp) and registers itself; da_native_caps_on_device applies the masks.
using DaNativeProbeReader = void (*)(CHWNativeCaps& caps);
void da_native_caps_register_probe_reader(DaNativeProbeReader reader);
} // namespace xray::render::RENDER_NAMESPACE
