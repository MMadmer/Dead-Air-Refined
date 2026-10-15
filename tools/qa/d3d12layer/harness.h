// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Differential tests of xrD3D12 (src/Layers/xrRenderDX12): every scenario runs on a real
// Direct3D 11 device and on a layer device of the same adapter; the images and values each run
// produces are read back and compared. A D3D12 debug layer error during a scenario fails it too.
#pragma once

#include "d12_layer.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace layertest
{
using Microsoft::WRL::ComPtr;
using xrd3d12::u16;
using xrd3d12::u32;
using xrd3d12::u64;
using xrd3d12::u8;

// One of the two devices a scenario runs on.
struct Gpu
{
    const char* name = "";
    bool layer = false;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
};

// A texture subresource read back into tightly packed rows.
struct Image
{
    std::string label;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    u32 width = 0;
    u32 height = 0;
    u32 depth = 1;
    u32 texelSize = 0;
    std::vector<u8> texels;
    // Per component: 8-bit UNORM in steps of 1/255, float formats absolute.
    double tolerance = 0;
};

// What one run of a scenario produced on one device.
class Result
{
public:
    std::vector<Image> images;
    std::vector<std::pair<std::string, long long>> values; // compared exactly between the devices
    std::vector<std::string> failures;                     // failures of this device alone

    void Value(const char* name, long long value) { values.emplace_back(name, value); }
    void Fail(const char* fmt, ...);
    // Records a failure when `condition` is false; returns the condition.
    bool Check(bool condition, const char* fmt, ...);
    // Records a failure for a failed HRESULT; returns SUCCEEDED(hr).
    bool Ok(HRESULT hr, const char* what);
};

// ---- shaders ---------------------------------------------------------------------------------

// Compiled once per (source, entry, profile) and kept; a compile error ends the program.
ID3DBlob* Compile(const char* source, const char* entry, const char* profile);
// The same through the layer's DXC service (d12_dxc.cpp): `profile` may be an FXC one ("vs_5_0" is
// built as "vs_6_0"); HLSL 2018, row-major like Compile().
const std::vector<unsigned char>& CompileDxil(const char* source, const char* entry, const char* profile);

// -dxil: the layer device gets every scenario shader as DXIL; the D3D11 reference keeps FXC's DXBC.
extern bool g_dxil;
struct ShaderCode
{
    const void* data = nullptr;
    size_t size = 0;
};
// What MakeVs & co. create on `g`: DXBC, or DXIL on the layer in -dxil mode.
ShaderCode Code(const Gpu& g, const char* source, const char* entry, const char* profile);
ComPtr<ID3D11VertexShader> MakeVs(Gpu& g, Result& r, const char* source, const char* entry);
ComPtr<ID3D11PixelShader> MakePs(Gpu& g, Result& r, const char* source, const char* entry);
ComPtr<ID3D11GeometryShader> MakeGs(Gpu& g, Result& r, const char* source, const char* entry);
ComPtr<ID3D11HullShader> MakeHs(Gpu& g, Result& r, const char* source, const char* entry);
ComPtr<ID3D11DomainShader> MakeDs(Gpu& g, Result& r, const char* source, const char* entry);
ComPtr<ID3D11ComputeShader> MakeCs(Gpu& g, Result& r, const char* source, const char* entry);

// ---- resources -------------------------------------------------------------------------------

D3D11_TEXTURE2D_DESC Desc2D(u32 width, u32 height, DXGI_FORMAT format, UINT bind, u32 mips = 1, u32 slices = 1);
ComPtr<ID3D11Texture2D> MakeTexture2D(Gpu& g, Result& r, const D3D11_TEXTURE2D_DESC& desc,
    const D3D11_SUBRESOURCE_DATA* data = nullptr);
ComPtr<ID3D11Buffer> MakeBuffer(Gpu& g, Result& r, UINT size, D3D11_USAGE usage, UINT bind, const void* data = nullptr);
ComPtr<ID3D11RenderTargetView> MakeRtv(Gpu& g, Result& r, ID3D11Resource* resource,
    const D3D11_RENDER_TARGET_VIEW_DESC* desc = nullptr);
ComPtr<ID3D11ShaderResourceView> MakeSrv(Gpu& g, Result& r, ID3D11Resource* resource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* desc = nullptr);
ComPtr<ID3D11DepthStencilView> MakeDsv(Gpu& g, Result& r, ID3D11Resource* resource,
    const D3D11_DEPTH_STENCIL_VIEW_DESC* desc = nullptr);
ComPtr<ID3D11SamplerState> MakeSampler(Gpu& g, Result& r, D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE address);

// Map(WRITE_DISCARD) + copy + Unmap.
void Upload(Gpu& g, ID3D11Resource* buffer, const void* data, size_t size);
void SetViewport(Gpu& g, float x, float y, float width, float height, float minDepth = 0, float maxDepth = 1);

// ---- readback --------------------------------------------------------------------------------

// Subresource `sub` of a (non-multisampled) 2D texture through a staging copy.
Image Read2D(Gpu& g, Result& r, ID3D11Texture2D* texture, u32 sub, const char* label, double tolerance = 0);
Image Read3D(Gpu& g, Result& r, ID3D11Texture3D* texture, const char* label, double tolerance = 0);

// ---- common shaders --------------------------------------------------------------------------

// QuadVs: a rectangle from the QuadConstants (6 vertices, triangle list, no input layout);
// FullscreenVs: one triangle over the target (3 vertices); ColorPs: QuadConstants::color;
// UvPs: (uv, 0.5, 1).
extern const char* const kQuadHlsl;

struct QuadConstants
{
    float rect[4];  // x0, y0, x1, y1 in NDC
    float color[4];
    float misc[4];  // x = depth, y > 0.5 mirrors the quad (back face)
};

// A rectangle in pixels of a width x height target.
QuadConstants QuadPx(float x0, float y0, float x1, float y1, u32 width, u32 height, float r, float g, float b, float a,
    float depth = 0.0f);

// ---- scenarios -------------------------------------------------------------------------------

using ScenarioFn = void (*)(Gpu& g, Result& r);

struct Scenario
{
    const char* name;
    ScenarioFn fn;
    bool layerOnly;
    bool referenceErrorsExpected = false; // the scenario provokes D3D11 errors on purpose
};

const std::vector<Scenario>& Scenarios();

// Scenario lists of the DirectX 12 feature lanes, one file each (docs/dead-air/dx12-features/PLAN.md
// C20); Scenarios() appends them in this order.
const std::vector<Scenario>& NativeScenarios();  // scenarios_native.cpp (infra)
const std::vector<Scenario>& ViewScenarios();    // scenarios_views.cpp (infra)
const std::vector<Scenario>& AsyncScenarios();   // scenarios_async.cpp (infra)
const std::vector<Scenario>& DxilScenarios();    // scenarios_dxil.cpp (infra)
const std::vector<Scenario>& VrsScenarios();     // scenarios_vrs.cpp (vrs)
const std::vector<Scenario>& MeshScenarios();    // scenarios_mesh.cpp (mesh)
const std::vector<Scenario>& TexheapScenarios(); // scenarios_texheap.cpp (ds)
const std::vector<Scenario>& RtScenarios();      // scenarios_rt.cpp (rt)

// The layer device: the harness drains its debug messages and ends frames through it.
xrd3d12::Device* LayerDevice();
// Lines the layer logged since the program started that contain `text`.
int LayerLogCount(const char* text);
// Ends a frame on either device (layer: submit + frame boundary; D3D11: flush).
void EndFrame(Gpu& g);
// Waits until the device finished every submitted command.
void WaitIdle(Gpu& g);
// The DXGI adapter both devices were created on.
IDXGIAdapter1* Adapter();

// Micro benchmark (not pass/fail): CPU milliseconds per frame of 20000 draws.
double BenchmarkFrameMs(Gpu& g, u32 frames, u32 drawsPerFrame);
// -profile: the layer's benchmark frames run under a sampling profiler (profiler.cpp).
extern bool g_profileBenchmark;
void ProfilerStart();
void ProfilerStop(const char* title, u32 top);
} // namespace layertest
