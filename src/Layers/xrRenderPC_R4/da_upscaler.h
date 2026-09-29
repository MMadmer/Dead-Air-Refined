// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt
//
// The upscale slot of the frame (docs/dead-air/DX12_RENDERER.md, 3.1-3.2): the interface the SDK
// backends (FSR on DX11 and DX12, DLSS through Streamline) implement, their registry, and the
// lifetime of the running one. Without an SDK the identity backend (a bilinear stretch) fills
// the slot, which proves the plumbing: render size, reactive mask, depth upscale, display-size
// post-processing.
#pragma once

namespace xray::render::RENDER_NAMESPACE
{
// Everything one upscale reads and writes. On the DX12 module the resources are xrD3D12 objects:
// the backends reach the native ones through xrd3d12::NativeResource.
struct UpscaleFrame
{
    ID3D11Resource* color{};    // render size, RGBA8, tonemapped (LDR), jittered
    ID3D11Resource* depth{};    // render size, R24G8 typeless, standard Z (0 = near), jittered
    ID3D11Resource* velocity{}; // render size, R16G16F: NDC(current) - NDC(previous), unjittered, y up; may be null
    ID3D11Resource* reactive{}; // render size, R8: 0 = opaque, towards 1 = transparent/particles
    ID3D11Resource* output{};   // display size, RGBA16F, UAV (and RTV)
    u32 renderW{}, renderH{};
    u32 displayW{}, displayH{};
    float jitterX{}, jitterY{}; // render pixels, the offset the projection carried (y down)
    float frameTimeMs{};
    float nearZ{}, farZ{};
    float fovY{};               // radians, vertical (deg2rad(Device.fFOV))
    bool reset{};               // discard the history
    float sharpness{};          // 0..1 (r__upscale_sharpness)
};

class IUpscaler
{
public:
    virtual ~IUpscaler() = default;
    // Creates the SDK context for this display size and quality (the r__fsr / r__dlss token value).
    virtual bool Init(u32 displayW, u32 displayH, int quality) = 0;
    virtual void Shutdown() = 0;
    // Records the upscale on the immediate context. false = the backend failed: the frame falls
    // back to the identity stretch and the jitter stops until the next renderer reset.
    virtual bool Dispatch(const UpscaleFrame& frame) = 0;
    // The render size for a quality at a display size (callable before Init).
    virtual void RenderSize(int quality, u32 displayW, u32 displayH, u32& renderW, u32& renderH) = 0;
    virtual pcstr Name() = 0;
    // Whether it reconstructs from jittered frames: every real upscaler does, the identity
    // stretch does not (it would only show the jitter as a shake).
    virtual bool Temporal() const { return true; }
};

enum class UpscalerKind : u32
{
    None = 0,
    FSR = 1,
    DLSS = 2,
};

// One SDK backend. The SDK stage registers them from its own translation units:
//     static da_upscaler_registrar reg{ { UpscalerKind::FSR, 11, "FSR 3.1 (DX11)", &create, &supported } };
struct UpscalerBackend
{
    UpscalerKind kind{};
    u32 api{};                   // 11 = xrRender_R4, 12 = xrRender_R5
    pcstr name{};
    IUpscaler* (*create)(){};    // a new instance, Init not called yet
    bool (*supported)(){};       // does this adapter run it (null = yes); called after the device exists
};
void da_upscaler_register(const UpscalerBackend& backend);

struct da_upscaler_registrar
{
    explicit da_upscaler_registrar(const UpscalerBackend& backend) { da_upscaler_register(backend); }
};

// What r__caps reports to the options menu. api and fsr come from here; the rest is filled by the
// hooks the SDK and frame generation stages register (all 0 until they do).
struct UpscalerCaps
{
    u32 api{};         // 11 or 12: the running renderer module
    bool dx12{};       // the adapter can run the DX12 renderer
    bool dlss{};       // DLSS Super Resolution / DLAA
    bool dlssg{};      // DLSS frame generation
    u32 dlssg_max{};   // frames DLSS can generate per rendered frame (1 = 2x, 3 = 4x)
    bool fsr{};        // FSR upscaling (every adapter)
    bool fsrfg{};      // FSR frame generation
    bool hags{};       // hardware-accelerated GPU scheduling is on
    // DirectX 12 features (docs/dead-air/dx12-features/PLAN.md 2.6), filled by da_native_caps.cpp.
    bool rt{};         // ray tracing usable this session
    u32 rt_tier{};     // DXR tier 0/10/11/12 (R4: the adapter's, from the D3D12 probe)
    u32 vrs_tier{};    // variable rate shading tier 0/1/2
    u32 vrs_tile{};    // tier 2: the rate image's tile size
    bool vrs{};        // variable rate shading runs
    bool ms{};         // mesh shaders tier 1 and DXC with SM 6.5
    u32 sm{};          // min(device, DXC) shader model as 60..69, 50 without DXC
    u32 wave{};        // wave lanes when wave operations run, else 0
    bool fp16{};       // native 16-bit shader operations
    bool eb{};         // enhanced barriers (information only)
    bool acq{};        // compute-queue jobs run on their own queue
    bool dxc{};        // engine shaders are DXIL this session
    bool gd{};         // the GPU-driven vegetation registry is live
    bool dstorage{};   // DirectStorage runtime and queues are up
};
using UpscalerCapsHook = void (*)(UpscalerCaps& caps);
void da_upscaler_register_caps_hook(UpscalerCapsHook hook);
UpscalerCaps da_upscaler_caps();

// Whether the adapter runs this kind on the running API: 1 yes, 0 no, -1 unknown (no backend of
// that kind is registered for this API, so nothing can tell - the setting is then allowed).
int da_upscaler_support(UpscalerKind kind);
// The running renderer's API: 11 or 12.
u32 da_render_api();

// The ratios of the quality tokens when a backend does not answer itself (FSR 1.0/1.5/1.7/2.0/3.0,
// DLSS DLAA 1.0 and 1.5/1.72/2.0/3.0), even-rounded, at least 64, never above the display size.
void da_upscaler_default_render_size(UpscalerKind kind, int quality, u32 displayW, u32 displayH, u32& renderW,
    u32& renderH);

// ---- lifetime (r2.cpp, the render target) -------------------------------------------------------

// Picks the backend for r__fsr / r__dlss and sets Device.dwRenderWidth/dwRenderHeight. First
// thing the render target does, before any target is created.
void da_update_render_size();
// Initialises the picked backend after the targets exist. false = it failed: the upscaler is off
// until its setting changes and the caller rebuilds the targets at display size.
bool da_upscaler_create();
// Shuts the backend down; before the targets go (reset_begin, destroy).
void da_upscaler_destroy();
// The running backend (null when none), and its runtime failure.
IUpscaler* da_upscaler_current();
void da_upscaler_fail(pcstr why);
// The r__fsr / r__dlss / r__aa settings changed: a failed backend gets another try.
void da_upscaler_settings_changed();
// The negative LOD bias the scene samplers get while the upscaler runs (0 without one).
float da_upscaler_mip_bias();
} // namespace xray::render::RENDER_NAMESPACE
