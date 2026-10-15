// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "grizzly_splash.h"

#ifdef XR_PLATFORM_WINDOWS
#include <SDL_syswm.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

// (The splash opens before the engine's core: nothing here takes the engine's allocator, log or file system.)

namespace
{
struct GrizzlyStroke
{
    float x0, y0, x1, y1, start;
};
#include "grizzly_emblem.inl"

constexpr u32 stroke_count = u32(std::size(grizzly_strokes));

// The look, one formula for the shader and the CPU. Lengths are in the emblem's frame (1 = half the window's
// height): the tube's half-width and the glow's near and far falloffs with their weights; the tube's core is
// nearly white, its glow violet.
constexpr float half_width = 0.0072f;
constexpr float glow_near = 0.011f, glow_near_k = 0.60f;
constexpr float glow_far = 0.05f, glow_far_k = 0.24f;
constexpr float hot[3] = { 0.93f, 0.80f, 1.0f };
constexpr float neon[3] = { 0.55f, 0.16f, 1.0f };
constexpr float exposure = 1.5f;

// The second the last stroke has been traced.
float lit_at()
{
    float last = 0.f;
    for (const GrizzlyStroke& s : grizzly_strokes)
        last = std::max(last, s.start);
    return last + grizzly_stroke_seconds;
}

// The tube's brightness: full while it lights up, three dips as it catches, then a slow breath.
float brightness(float t, float lit)
{
    const float c = t - lit;
    if (c < 0.f)
        return 1.f;
    if (c < 0.45f)
    {
        constexpr float dips[][3] = { { 0.04f, 0.09f, 0.25f }, { 0.15f, 0.18f, 0.55f }, { 0.30f, 0.34f, 0.35f } };
        for (const auto& d : dips)
            if (c >= d[0] && c < d[1])
                return d[2];
        return 1.f;
    }
    return 0.95f + 0.05f * std::cos((c - 0.45f) * 1.6f);
}

// A pixel of the lit emblem: px, py in the emblem's frame, aa a pixel there.
void shade(float px, float py, float aa, u8 rgb[3])
{
    float d = 1e9f;
    for (const GrizzlyStroke& s : grizzly_strokes)
    {
        const float bx = s.x1 - s.x0, by = s.y1 - s.y0;
        const float ax = px - s.x0, ay = py - s.y0;
        const float h = std::clamp((ax * bx + ay * by) / std::max(bx * bx + by * by, 1e-8f), 0.f, 1.f);
        d = std::min(d, std::hypot(ax - bx * h, ay - by * h));
    }
    const float core = std::clamp((half_width + aa - d) / (2.f * aa), 0.f, 1.f);
    const float glow = glow_near_k * std::exp(-d / glow_near) + glow_far_k * std::exp(-d / glow_far);
    for (int i = 0; i < 3; ++i)
        rgb[i] = u8(std::clamp(1.f - std::exp(-(core * hot[i] + glow * neon[i]) * exposure), 0.f, 1.f) * 255.f + 0.5f);
}

// The finished frame on the CPU, every core on its own rows, into the window's surface.
void draw_cpu(SDL_Window* window)
{
    SDL_Surface* surface = SDL_GetWindowSurface(window);
    if (!surface || surface->format->BytesPerPixel != 4)
        return;
    if (SDL_MUSTLOCK(surface))
        SDL_LockSurface(surface);
    const int w = surface->w, h = surface->h;
    const float unit = float(h) * 0.5f;
    const u32 threads = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> pool;
    for (u32 k = 0; k < threads; ++k)
    {
        pool.emplace_back([=]
        {
            for (int y = int(k); y < h; y += int(threads))
            {
                auto* row = reinterpret_cast<u32*>(static_cast<u8*>(surface->pixels) + y * surface->pitch);
                for (int x = 0; x < w; ++x)
                {
                    u8 rgb[3];
                    shade((float(x) + 0.5f - w * 0.5f) / unit, (h * 0.5f - float(y) - 0.5f) / unit, 1.f / unit, rgb);
                    row[x] = SDL_MapRGB(surface->format, rgb[0], rgb[1], rgb[2]);
                }
            }
        });
    }
    for (std::thread& t : pool)
        t.join();
    if (SDL_MUSTLOCK(surface))
        SDL_UnlockSurface(surface);
    SDL_UpdateWindowSurface(window);
}

#ifdef XR_PLATFORM_WINDOWS
// The pixel shader of the same formula, with the strokes as its constants and the trace in time: a stroke grows
// from its first end over grizzly_stroke_seconds from its start.
std::string shader_source()
{
    std::string s;
    string256 line;
    s += "cbuffer View : register(b0) { float4 g_view; };  // x, y: the size in pixels; z: the second; w: brightness\n";
    xr_sprintf(line, "static const uint N = %u;\nstatic const float STROKE = %.5f;\n", stroke_count, grizzly_stroke_seconds);
    s += line;
    s += "static const float4 S[N] = {\n";
    for (const GrizzlyStroke& k : grizzly_strokes)
    {
        xr_sprintf(line, "    float4(%.5f, %.5f, %.5f, %.5f),\n", k.x0, k.y0, k.x1, k.y1);
        s += line;
    }
    s += "};\nstatic const float T[N] = {\n";
    for (const GrizzlyStroke& k : grizzly_strokes)
    {
        xr_sprintf(line, "    %.5f,\n", k.start);
        s += line;
    }
    s += "};\n";
    xr_sprintf(line, "static const float HW = %.5f, GN = %.5f, GNK = %.5f, GF = %.5f, GFK = %.5f, EXPOSURE = %.5f;\n",
        half_width, glow_near, glow_near_k, glow_far, glow_far_k, exposure);
    s += line;
    xr_sprintf(line, "static const float3 HOT = float3(%.5f, %.5f, %.5f), NEON = float3(%.5f, %.5f, %.5f);\n", hot[0],
        hot[1], hot[2], neon[0], neon[1], neon[2]);
    s += line;
    s += R"(
float4 vs_main(uint id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 ps_main(float4 pos : SV_Position) : SV_Target
{
    const float unit = g_view.y * 0.5;
    const float2 p = float2(pos.x - g_view.x * 0.5, g_view.y * 0.5 - pos.y) / unit;
    float d = 1e9;
    [loop] for (uint i = 0; i < N; ++i)
    {
        const float k = saturate((g_view.z - T[i]) / STROKE);
        [branch] if (k > 0.0)
        {
            const float2 a = S[i].xy;
            const float2 ba = (S[i].zw - a) * k;
            const float2 pa = p - a;
            const float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-8));
            d = min(d, length(pa - ba * h));
        }
    }
    const float aa = 1.0 / unit;
    const float core = saturate((HW + aa - d) / (2.0 * aa));
    const float glow = GNK * exp(-d / GN) + GFK * exp(-d / GF);
    return float4(1.0 - exp(-(core * HOT + glow * NEON) * EXPOSURE * g_view.w), 1.0);
}
)";
    return s;
}

// -splash_capture (QA): the frames at these seconds into splash_<ms>.bmp in the working folder - a flip-model
// window on the QA rig's hidden desktop cannot be captured from outside.
void capture(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain1* chain, float t)
{
    ID3D11Texture2D* back{};
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&back))))
        return;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging{};
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)))
    {
        context->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            char name[64];
            std::snprintf(name, sizeof(name), "splash_%05d.bmp", int(t * 1000.f));
            if (FILE* f = std::fopen(name, "wb"))
            {
                const u32 row = desc.Width * 4, size = row * desc.Height;
                BITMAPFILEHEADER fh{ 0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + size), 0, 0,
                    DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) };
                BITMAPINFOHEADER ih{ sizeof(BITMAPINFOHEADER), LONG(desc.Width), -LONG(desc.Height), 1, 32, BI_RGB, size };
                std::fwrite(&fh, sizeof(fh), 1, f);
                std::fwrite(&ih, sizeof(ih), 1, f);
                for (u32 y = 0; y < desc.Height; ++y)
                    std::fwrite(static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch, row, 1, f);
                std::fclose(f);
            }
            context->Unmap(staging, 0);
        }
        staging->Release();
    }
    back->Release();
}

template <class T>
void release(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

// The animated splash through a Direct3D 11 device of its own; false where there is none (nothing is drawn then).
bool run_d3d11(SDL_Window* window, const std::atomic_bool& should_exit, const std::function<void()>& idle)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info))
        return false;
    const HWND hwnd = info.info.win.window;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const UINT width = UINT(rc.right - rc.left), height = UINT(rc.bottom - rc.top);
    if (!width || !height)
        return false;

    const HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11CreateDevice")) : nullptr;
    const auto compile = compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile")) : nullptr;

    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    IDXGIFactory2* factory{};
    IDXGISwapChain1* chain{};
    ID3D11RenderTargetView* target{};
    ID3D11VertexShader* vs{};
    ID3D11PixelShader* ps{};
    ID3D11Buffer* view{};
    bool ok = false;
    do
    {
        if (!create || !compile)
            break;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        if (FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                UINT(std::size(levels)), D3D11_SDK_VERSION, &device, nullptr, &context)))
            break;
        {
            IDXGIDevice* dxgi{};
            IDXGIAdapter* adapter{};
            const bool got = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) &&
                SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)));
            release(adapter);
            release(dxgi);
            if (!got)
                break;
        }
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(device, hwnd, &desc, nullptr, nullptr, &chain)))
            break;
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        {
            ID3D11Texture2D* back{};
            if (SUCCEEDED(chain->GetBuffer(0, IID_PPV_ARGS(&back))))
                device->CreateRenderTargetView(back, nullptr, &target);
            release(back);
            if (!target)
                break;
        }
        const std::string source = shader_source();
        ID3DBlob* vs_code{};
        ID3DBlob* ps_code{};
        ID3DBlob* errors{};
        const bool built =
            SUCCEEDED(compile(source.data(), source.size(), "grizzly_splash", nullptr, nullptr, "vs_main", "vs_4_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs_code, &errors)) &&
            SUCCEEDED(compile(source.data(), source.size(), "grizzly_splash", nullptr, nullptr, "ps_main", "ps_4_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps_code, &errors));
        if (!built && errors)
            OutputDebugStringA(static_cast<pcstr>(errors->GetBufferPointer()));
        if (built)
        {
            device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs);
            device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps);
        }
        release(vs_code);
        release(ps_code);
        release(errors);
        if (!vs || !ps)
            break;
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 16;
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cb, nullptr, &view)))
            break;
        ok = true;
    } while (false);

    if (ok)
    {
        const float lit = lit_at();
        // (the command line straight from Windows: the engine has not parsed it yet)
        const bool captures = strstr(GetCommandLineA(), "-splash_capture") != nullptr;
        const float capture_at[] = { 0.45f, 0.9f, lit - 0.05f, lit + 0.1f, lit + 1.0f };
        u32 captured = 0;
        const auto started = std::chrono::steady_clock::now();
        auto idled = started;
        const D3D11_VIEWPORT viewport{ 0.f, 0.f, float(width), float(height), 0.f, 1.f };
        while (!should_exit.load(std::memory_order_acquire))
        {
            const auto now = std::chrono::steady_clock::now();
            const float t = std::chrono::duration<float>(now - started).count();
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(view, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                const float values[4] = { float(width), float(height), t, brightness(t, lit) };
                CopyMemory(mapped.pData, values, sizeof(values));
                context->Unmap(view, 0);
            }
            context->OMSetRenderTargets(1, &target, nullptr);
            context->RSSetViewports(1, &viewport);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vs, nullptr, 0);
            context->PSSetShader(ps, nullptr, 0);
            context->PSSetConstantBuffers(0, 1, &view);
            context->Draw(3, 0);
            if (captures && captured < std::size(capture_at) && t >= capture_at[captured])
                capture(device, context, chain, capture_at[captured++]);
            chain->Present(1, 0);
            if (now - idled >= std::chrono::milliseconds(30))
            {
                idle();
                idled = now;
            }
            // (Once lit, a breath at a few frames a second is all it shows: the loading behind gets the machine.)
            if (t > lit + 0.6f)
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    if (context)
        context->ClearState();
    release(view);
    release(ps);
    release(vs);
    release(target);
    release(chain);
    release(factory);
    release(context);
    release(device);
    if (compiler)
        FreeLibrary(compiler);
    if (d3d11)
        FreeLibrary(d3d11);
    return ok;
}
#endif
} // namespace

void grizzly_splash_size(int& width, int& height)
{
    SDL_DisplayMode mode{};
    if (SDL_GetDesktopDisplayMode(0, &mode) != 0 || mode.w <= 0 || mode.h <= 0)
    {
        mode.w = 1920;
        mode.h = 1080;
    }
    height = std::clamp(int(std::lround(mode.h * 0.46)), 200, mode.h);
    width = std::min(int(std::lround(height * 1.6)), int(mode.w * 0.9));
}

void grizzly_splash_run(SDL_Window* window, const std::atomic_bool& should_exit, const std::function<void()>& idle)
{
    ZoneScopedN("Grizzly splash");
#ifdef XR_PLATFORM_WINDOWS
    if (run_d3d11(window, should_exit, idle))
        return;
#endif
    draw_cpu(window);
    while (!should_exit.load(std::memory_order_acquire))
    {
        idle();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}
