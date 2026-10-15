// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "da_loading_presenter.h"

#include "da_media.h"
#include "xrTheora_Surface.h"

#if defined(XR_PLATFORM_WINDOWS)
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <mfapi.h>
// The media engine's declarations are Windows 8's; the engine's headers are set for 7 (the game itself runs on 10
// and later, where the engine is always there).
#pragma push_macro("WINVER")
#undef WINVER
#define WINVER 0x0A00
#include <mfmediaengine.h>
#pragma pop_macro("WINVER")
#endif

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <regex>
#include <thread>
#include <unordered_map>

int psLoadingScreenAsync = 1;
int psLoadingScreenCapture = 0;

#if defined(XR_PLATFORM_WINDOWS)
namespace
{
template <class T>
void release(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

std::wstring widen(pcstr s)
{
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? size_t(n - 1) : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

// ---- the device's own shaders --------------------------------------------------------------------------------

// A quad of a rectangle in screen pixels, its texture rectangle and colour: the layout of the game's
// stub_notransform_t output (v2p_TL), so a mod's pixel shader written for the game takes it as it is.
constexpr char quad_vs[] = R"(
cbuffer quad : register(b0) { float4 rect; float4 uv; float4 color; float4 screen; };
struct v2p { float2 Tex0 : TEXCOORD0; float4 Color : COLOR; float4 HPos : SV_Position; };
v2p main(uint id : SV_VertexID)
{
    float2 k = float2(id & 1, id >> 1);
    float2 p = lerp(rect.xy, rect.zw, k);
    v2p o;
    o.HPos = float4(p.x * screen.z * 2.0 - 1.0, 1.0 - p.y * screen.w * 2.0, 0.0, 1.0);
    o.Tex0 = lerp(uv.xy, uv.zw, k);
    o.Color = color;
    return o;
})";

// a picture or a frame, straight alpha, tinted
constexpr char picture_ps[] = R"(
Texture2D s_base : register(t0);
SamplerState smp : register(s0);
struct v2p { float2 Tex0 : TEXCOORD0; float4 Color : COLOR; float4 HPos : SV_Position; };
float4 main(v2p I) : SV_Target { return s_base.Sample(smp, I.Tex0) * I.Color; })";

struct QuadConstants
{
    Fvector4 rect, uv, color, screen;
};

Fvector4 rgba(u32 argb)
{
    return { color_get_R(argb) / 255.f, color_get_G(argb) / 255.f, color_get_B(argb) / 255.f, color_get_A(argb) / 255.f };
}

// ---- a texture of the device -----------------------------------------------------------------------------------

struct GpuTexture
{
    ID3D11Texture2D* texture{};
    ID3D11ShaderResourceView* view{};
    u32 width{}, height{};

    void reset()
    {
        release(view);
        release(texture);
        width = height = 0;
    }
    explicit operator bool() const { return view != nullptr; }
};

bool make_texture(ID3D11Device* device, u32 w, u32 h, DXGI_FORMAT format, const void* pixels, u32 pitch, bool dynamic,
    GpuTexture& out, UINT extraBind = 0)
{
    out.reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | extraBind;
    desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    D3D11_SUBRESOURCE_DATA data{ pixels, pitch, 0 };
    if (FAILED(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &out.texture)) ||
        FAILED(device->CreateShaderResourceView(out.texture, nullptr, &out.view)))
    {
        out.reset();
        return false;
    }
    out.width = w;
    out.height = h;
    return true;
}

void upload(ID3D11DeviceContext* context, GpuTexture& t, const u8* pixels, size_t rowBytes, u32 rows)
{
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(context->Map(t.texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return;
    for (u32 y = 0; y < rows; ++y)
        CopyMemory(static_cast<u8*>(map.pData) + size_t(y) * map.RowPitch, pixels + y * rowBytes, rowBytes);
    context->Unmap(t.texture, 0);
}

// A .dds of the file system - the formats the game's interface uses - with its mips.
bool load_dds(ID3D11Device* device, pcstr name, GpuTexture& out)
{
    string_path fn;
    if (!FS.exist(fn, "$game_textures$", name, ".dds"))
        return false;
    IReader* file = FS.r_open(fn);
    if (!file)
        return false;
    const u8* data = static_cast<const u8*>(file->pointer());
    const size_t size = file->length();
    bool ok = false;
    if (size >= 128 && *reinterpret_cast<const u32*>(data) == 0x20534444u)
    {
        const u32* h = reinterpret_cast<const u32*>(data + 4);
        const u32 height = h[2], width = h[3], mips = std::max(h[6], 1u);
        const u32 pfFlags = h[19], fourcc = h[20], bits = h[21], rmask = h[22];
        size_t offset = 128;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        u32 block = 0; // bytes per 4x4 block, 0 for a plain format
        u32 bpp = 0;
        const auto cc = [](char a, char b, char c, char d) { return u32(u8(a)) | u32(u8(b)) << 8 | u32(u8(c)) << 16 | u32(u8(d)) << 24; };
        if (pfFlags & 4)
        {
            if (fourcc == cc('D', 'X', 'T', '1'))
                format = DXGI_FORMAT_BC1_UNORM, block = 8;
            else if (fourcc == cc('D', 'X', 'T', '3'))
                format = DXGI_FORMAT_BC2_UNORM, block = 16;
            else if (fourcc == cc('D', 'X', 'T', '5'))
                format = DXGI_FORMAT_BC3_UNORM, block = 16;
            else if (fourcc == cc('A', 'T', 'I', '1') || fourcc == cc('B', 'C', '4', 'U'))
                format = DXGI_FORMAT_BC4_UNORM, block = 8;
            else if (fourcc == cc('A', 'T', 'I', '2') || fourcc == cc('B', 'C', '5', 'U'))
                format = DXGI_FORMAT_BC5_UNORM, block = 16;
            else if (fourcc == cc('D', 'X', '1', '0') && size >= 148)
            {
                format = DXGI_FORMAT(*reinterpret_cast<const u32*>(data + 128));
                offset = 148;
                switch (format)
                {
                case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_UNORM: block = 8; break;
                case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM:
                case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC6H_UF16:
                case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: block = 16; break;
                case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM: bpp = 32; break;
                default: format = DXGI_FORMAT_UNKNOWN;
                }
            }
        }
        else if ((pfFlags & 0x40) && bits == 32)
        {
            format = rmask == 0x000000FFu ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            bpp = 32;
        }
        if (format != DXGI_FORMAT_UNKNOWN)
        {
            xr_vector<D3D11_SUBRESOURCE_DATA> levels;
            u32 w = width, hh = height;
            for (u32 m = 0; m < mips; ++m)
            {
                const u32 pitch = block ? std::max(1u, (w + 3) / 4) * block : w * bpp / 8;
                const u32 rows = block ? std::max(1u, (hh + 3) / 4) : hh;
                if (offset + size_t(pitch) * rows > size)
                    break;
                levels.push_back({ data + offset, pitch, 0 });
                offset += size_t(pitch) * rows;
                w = std::max(1u, w / 2);
                hh = std::max(1u, hh / 2);
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = u32(levels.size());
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            out.reset();
            ok = !levels.empty() && SUCCEEDED(device->CreateTexture2D(&desc, levels.data(), &out.texture)) &&
                SUCCEEDED(device->CreateShaderResourceView(out.texture, nullptr, &out.view));
            if (ok)
                out.width = width, out.height = height;
            else
                out.reset();
        }
    }
    FS.r_close(file);
    return ok;
}

// ---- what moves: a video with its sound, a GIF, a Theora video ---------------------------------------------

struct Source
{
    virtual ~Source() = default;
    // the frame for the time (da_media_time), or null while there is none yet
    virtual ID3D11ShaderResourceView* frame(ID3D11DeviceContext* context, u32 time) = 0;
    virtual void volume(float) {}
};

// A video through the Media Foundation media engine: decoded on the GPU of this device, its sound played by the
// engine itself, looped.
class VideoSource final : public Source, public IMFMediaEngineNotify
{
    std::atomic<ULONG> m_refs{ 1 };
    IMFMediaEngine* m_engine{};
    GpuTexture m_frame;
    ID3D11Device* m_device;
    xr_string m_name;

public:
    explicit VideoSource(ID3D11Device* device) : m_device(device) {}
    ~VideoSource() override
    {
        if (m_engine)
            m_engine->Shutdown();
        release(m_engine);
        m_frame.reset();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify))
        {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    // (Owned by the presenter; the engine's references come and go with it.)
    ULONG STDMETHODCALLTYPE Release() override { return --m_refs; }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR, DWORD) override
    {
        if (event == MF_MEDIA_ENGINE_EVENT_ERROR)
            Msg("! [loading] %s: the video cannot be played", m_name.c_str());
        return S_OK;
    }

    bool open(IMFDXGIDeviceManager* manager, pcstr fn, float vol)
    {
        m_name = fn;
        IMFMediaEngineClassFactory* factory = nullptr;
        IMFAttributes* attributes = nullptr;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(MFCreateAttributes(&attributes, 3)) &&
            SUCCEEDED(attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager)) &&
            SUCCEEDED(attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, static_cast<IMFMediaEngineNotify*>(this))) &&
            SUCCEEDED(attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM)) &&
            SUCCEEDED(factory->CreateInstance(0, attributes, &m_engine));
        release(attributes);
        release(factory);
        if (!ok)
            return false;

        // A loose file plays from its path; one of an archive through a stream over the archive's bytes.
        const std::wstring name = widen(fn);
        BSTR url = SysAllocString(name.c_str());
        const CLocatorAPI::file* desc = FS.GetFileDesc(fn);
        if (desc && desc->vfs == std::numeric_limits<size_t>::max())
            ok = SUCCEEDED(m_engine->SetSource(url));
        else
        {
            IMFMediaEngineEx* ex = nullptr;
            IMFByteStream* bytes = nullptr;
            IStream* stream = da_media_open_stream(fn);
            ok = stream && SUCCEEDED(m_engine->QueryInterface(IID_PPV_ARGS(&ex))) &&
                SUCCEEDED(MFCreateMFByteStreamOnStream(stream, &bytes)) && SUCCEEDED(ex->SetSourceFromByteStream(bytes, url));
            release(bytes);
            release(ex);
            if (stream)
                stream->Release();
        }
        SysFreeString(url);
        if (!ok)
            return false;
        m_engine->SetLoop(TRUE);
        m_engine->SetVolume(vol);
        return SUCCEEDED(m_engine->Play());
    }

    ID3D11ShaderResourceView* frame(ID3D11DeviceContext*, u32) override
    {
        LONGLONG pts;
        if (m_engine->OnVideoStreamTick(&pts) == S_OK)
        {
            DWORD w = 0, h = 0;
            if (SUCCEEDED(m_engine->GetNativeVideoSize(&w, &h)) && w && h)
            {
                if (m_frame.width != w || m_frame.height != h)
                    make_texture(m_device, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, nullptr, 0, false, m_frame, D3D11_BIND_RENDER_TARGET);
                if (m_frame)
                {
                    const MFVideoNormalizedRect src{ 0.f, 0.f, 1.f, 1.f };
                    const RECT dst{ 0, 0, LONG(w), LONG(h) };
                    const MFARGB border{ 0, 0, 0, 255 };
                    m_engine->TransferVideoFrame(m_frame.texture, &src, &dst, &border);
                }
            }
        }
        return m_frame.view;
    }

    void volume(float v) override { m_engine->SetVolume(v); }
};

// a GIF of da_media.cpp, its frames uploaded as they change
class GifSource final : public Source
{
    CDaMediaSource* m_gif;
    GpuTexture m_frame;

public:
    GifSource(ID3D11Device* device, CDaMediaSource* gif) : m_gif(gif)
    {
        make_texture(device, gif->Width(), gif->Height(), DXGI_FORMAT_R8G8B8A8_UNORM, gif->Pixels(), gif->Width() * 4, true, m_frame);
    }
    ~GifSource() override
    {
        m_frame.reset();
        xr_delete(m_gif);
    }
    ID3D11ShaderResourceView* frame(ID3D11DeviceContext* context, u32 time) override
    {
        if (m_frame && m_gif->Update(time))
            upload(context, m_frame, m_gif->Pixels(), size_t(m_gif->Width()) * 4, m_gif->Height());
        return m_frame.view;
    }
};

// A Theora video as the game plays it, its frame in the corner of a power-of-two texture like the game's, in
// colour (the game turns its YUV into colour in a shader of its own).
class TheoraSource final : public Source
{
    CTheoraSurface* m_theora;
    GpuTexture m_frame;

public:
    TheoraSource(ID3D11Device* device, CTheoraSurface* theora) : m_theora(theora)
    {
        make_texture(device, theora->Width(false), theora->Height(false), DXGI_FORMAT_B8G8R8A8_UNORM, nullptr, 0, true, m_frame);
    }
    ~TheoraSource() override
    {
        m_frame.reset();
        xr_delete(m_theora);
    }
    ID3D11ShaderResourceView* frame(ID3D11DeviceContext* context, u32 time) override
    {
        if (m_frame && m_theora->Update(time))
        {
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(context->Map(m_frame.texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
            {
                int pos = 0;
                m_theora->DecompressFrame(static_cast<u32*>(map.pData), map.RowPitch / 4 - m_theora->Width(true), pos);
                context->Unmap(m_frame.texture, 0);
            }
        }
        return m_frame.view;
    }
};

// ---- a mod's shader ----------------------------------------------------------------------------------------

// The include files of the game's shaders, read through the file system like the game reads them.
class ShaderIncludes final : public ID3DInclude
{
public:
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override
    {
        string_path path;
        strconcat(path, "r3\\", name);
        IReader* file = FS.r_open("$game_shaders$", path);
        if (!file)
            return E_FAIL;
        auto* copy = xr_alloc<u8>(file->length() + 1);
        CopyMemory(copy, file->pointer(), file->length());
        copy[file->length()] = 0;
        *data = copy;
        *bytes = UINT(file->length());
        FS.r_close(file);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override
    {
        xr_free(data);
        return S_OK;
    }
};

using PFN_COMPILE = decltype(&D3DCompile);
using PFN_REFLECT = decltype(&D3DReflect);

// A static's shader as its .s describes it: the pixel shader, the blend, the textures beside the static's own.
struct Program
{
    ID3D11PixelShader* ps{};
    ID3D11BlendState* blend{};
    ID3D11Buffer* globals{};
    xr_vector<u8> globalsData;
    struct Var
    {
        xr_string name;
        u32 offset, size;
    };
    xr_vector<Var> vars;
    u32 baseSlot{ ~0u };
    struct Extra
    {
        u32 slot;
        GpuTexture texture;
    };
    xr_vector<Extra> extras;
    struct Sampler
    {
        u32 slot;
        bool wrap, point;
    };
    xr_vector<Sampler> samplers;

    ~Program()
    {
        release(ps);
        release(blend);
        release(globals);
        for (Extra& e : extras)
            e.texture.reset();
    }

    void set(pcstr name, const Fvector4& v)
    {
        for (const Var& var : vars)
            if (var.name == name)
                CopyMemory(&globalsData[var.offset], &v, std::min<size_t>(var.size, sizeof(v)));
    }
};

D3D11_BLEND blend_factor(const xr_string& s, D3D11_BLEND fallback)
{
    static const std::pair<pcstr, D3D11_BLEND> table[] = { { "zero", D3D11_BLEND_ZERO }, { "one", D3D11_BLEND_ONE },
        { "srccolor", D3D11_BLEND_SRC_COLOR }, { "invsrccolor", D3D11_BLEND_INV_SRC_COLOR },
        { "srcalpha", D3D11_BLEND_SRC_ALPHA }, { "invsrcalpha", D3D11_BLEND_INV_SRC_ALPHA },
        { "destalpha", D3D11_BLEND_DEST_ALPHA }, { "invdestalpha", D3D11_BLEND_INV_DEST_ALPHA },
        { "destcolor", D3D11_BLEND_DEST_COLOR }, { "invdestcolor", D3D11_BLEND_INV_DEST_COLOR } };
    for (const auto& [name, factor] : table)
        if (s == name)
            return factor;
    return fallback;
}

// ---- the presenter -------------------------------------------------------------------------------------------

struct LayerState
{
    u32 revision{ ~0u };
    GpuTexture picture;
    xr_vector<GpuTexture> percents;
    std::unique_ptr<Source> source;
    GpuTexture image; // a Shader layer's own texture where it is a picture
    std::unique_ptr<Program> program;
    bool tried{};

    void reset()
    {
        picture.reset();
        for (GpuTexture& p : percents)
            p.reset();
        percents.clear();
        source.reset();
        image.reset();
        program.reset();
        tried = false;
        revision = ~0u;
    }
};

class Presenter
{
public:
    // shared with the main thread
    std::mutex lock;
    std::condition_variable wake;
    DaLoadingScene scene;
    u32 sceneVersion{};
    bool want{}; // shown
    bool quit{};
    std::atomic<float> progress{ 0.f };
    HWND game{};
    bool failed{};

private:
    std::thread m_thread;

    // the thread's own
    HMODULE m_d3d11{}, m_compiler{};
    PFN_COMPILE m_compile{};
    PFN_REFLECT m_reflect{};
    ID3D11Device* m_device{};
    ID3D11DeviceContext* m_context{};
    IDXGIFactory2* m_factory{};
    IDXGISwapChain1* m_chain{};
    ID3D11RenderTargetView* m_target{};
    ID3D11VertexShader* m_vs{};
    ID3D11PixelShader* m_ps{};
    ID3D11Buffer* m_quad{};
    ID3D11BlendState* m_alpha{};
    ID3D11RasterizerState* m_raster{};
    ID3D11SamplerState* m_clamp{};
    ID3D11SamplerState* m_wrap{};
    ID3D11SamplerState* m_point{};
    IMFDXGIDeviceManager* m_manager{};
    bool m_mf{};
    HWND m_window{};
    RECT m_rect{};
    u32 m_width{}, m_height{};
    DaLoadingScene m_scene;
    u32 m_version{ ~0u };
    std::unordered_map<u32, LayerState> m_layers;
    bool m_shown{};
    float m_eased{};
    u32 m_easedAt{};
    u32 m_opened{};
    // the frames of a loading: how many, the longest wait between two
    u32 m_frames{}, m_lastFrame{}, m_longest{}, m_slow{};
    // a video's sound fading out after the window went
    u32 m_fadeFrom{};
    // loading_screen_capture: frames of its own saved four times a second, for the QA of what the game cannot
    // capture (its r__capture takes its own frames)
    u32 m_captured{}, m_capturedAt{};

public:
    string_path captureDir{};

private:
    void capture(u32 time)
    {
        if (!psLoadingScreenCapture || !captureDir[0] || (m_captured && time - m_capturedAt < 250))
            return;
        ID3D11Texture2D* back = nullptr;
        if (FAILED(m_chain->GetBuffer(0, IID_PPV_ARGS(&back))))
            return;
        D3D11_TEXTURE2D_DESC desc;
        back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        if (SUCCEEDED(m_device->CreateTexture2D(&desc, nullptr, &staging)))
        {
            m_context->CopyResource(staging, back);
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(m_context->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
            {
                string_path fn;
                xr_sprintf(fn, "%sframe_%03u.tga", captureDir, m_captured);
                if (FILE* f = fopen(fn, "wb"))
                {
                    // uncompressed 32-bit, top row first
                    u8 header[18]{};
                    header[2] = 2;
                    header[12] = u8(desc.Width & 0xFF), header[13] = u8(desc.Width >> 8);
                    header[14] = u8(desc.Height & 0xFF), header[15] = u8(desc.Height >> 8);
                    header[16] = 32;
                    header[17] = 0x28;
                    fwrite(header, 1, sizeof(header), f);
                    for (u32 y = 0; y < desc.Height; ++y)
                        fwrite(static_cast<const u8*>(map.pData) + size_t(y) * map.RowPitch, 4, desc.Width, f);
                    fclose(f);
                }
                m_context->Unmap(staging, 0);
            }
            release(staging);
        }
        release(back);
        m_capturedAt = time;
        if (++m_captured >= u32(psLoadingScreenCapture))
            m_captured = 0, psLoadingScreenCapture = 0;
    }

public:
    bool start()
    {
        m_thread = std::thread([this] { run(); });
        std::unique_lock guard(lock);
        wake.wait(guard, [&] { return m_device || failed; });
        return !failed;
    }

    void stop()
    {
        {
            std::lock_guard guard(lock);
            quit = true;
        }
        wake.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
        case WM_ERASEBKGND: return 1;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    bool init()
    {
        // A loading keeps every core busy: the screen's thread comes before its workers.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        m_compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        const auto create = m_d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(m_d3d11, "D3D11CreateDevice")) : nullptr;
        m_compile = m_compiler ? reinterpret_cast<PFN_COMPILE>(GetProcAddress(m_compiler, "D3DCompile")) : nullptr;
        m_reflect = m_compiler ? reinterpret_cast<PFN_REFLECT>(GetProcAddress(m_compiler, "D3DReflect")) : nullptr;
        if (!create || !m_compile || !m_reflect)
            return false;

        // A device of its own: the game's never waits on it, nor it on the game's. Video support for the
        // media engine's decoding where the driver has it.
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        if (FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, UINT(std::size(levels)), D3D11_SDK_VERSION,
                &m_device, nullptr, &m_context)))
        {
            flags &= ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            if (FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, UINT(std::size(levels)),
                    D3D11_SDK_VERSION, &m_device, nullptr, &m_context)))
                return false;
        }
        ID3D10Multithread* multithread = nullptr;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&multithread))))
        {
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
        }
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        const bool factory = SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&m_factory)));
        release(adapter);
        release(dxgi);
        if (!factory)
            return false;

        ID3DBlob* vs = compile(quad_vs, "vs_5_0", "loading quad", nullptr);
        ID3DBlob* ps = compile(picture_ps, "ps_5_0", "loading picture", nullptr);
        const bool shaders = vs && ps && SUCCEEDED(m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs)) &&
            SUCCEEDED(m_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps));
        release(vs);
        release(ps);
        if (!shaders)
            return false;

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(QuadConstants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_device->CreateBuffer(&cb, nullptr, &m_quad);
        m_alpha = make_blend(D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA);
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.ScissorEnable = TRUE;
        m_device->CreateRasterizerState(&rs, &m_raster);
        m_clamp = make_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP);
        m_wrap = make_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP);
        m_point = make_sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP);

        // Media Foundation where the system has it (a Windows N without its Media Feature Pack has not): a video
        // shows as its first frame drawn by the game then.
        if ((flags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) && LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) &&
            SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
        {
            UINT token = 0;
            m_mf = SUCCEEDED(MFCreateDXGIDeviceManager(&token, &m_manager)) && SUCCEEDED(m_manager->ResetDevice(m_device, token));
        }

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"GrizzlyLoadingScreen";
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassExW(&wc);
        // Not owned by the game's window: an owner on another thread would tie this thread's messages to that
        // one's, the very wait this window is there to avoid.
        m_window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr,
            nullptr, wc.hInstance, nullptr);
        return m_window != nullptr;
    }

    void shutdown()
    {
        m_layers.clear();
        release(m_target);
        release(m_chain);
        if (m_window)
            DestroyWindow(m_window);
        release(m_manager);
        if (m_mf)
            MFShutdown();
        release(m_point);
        release(m_wrap);
        release(m_clamp);
        release(m_raster);
        release(m_alpha);
        release(m_quad);
        release(m_ps);
        release(m_vs);
        release(m_factory);
        release(m_context);
        release(m_device);
        CoUninitialize();
    }

    ID3DBlob* compile(pcstr source, pcstr target, pcstr name, ID3DInclude* include)
    {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT hr = m_compile(source, xr_strlen(source), name, nullptr, include, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr))
            Msg("! [loading] %s: %s", name, errors ? static_cast<pcstr>(errors->GetBufferPointer()) : "not compiled");
        release(errors);
        return SUCCEEDED(hr) ? code : nullptr;
    }

    ID3D11BlendState* make_blend(D3D11_BLEND src, D3D11_BLEND dst)
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = src;
        bd.RenderTarget[0].DestBlend = dst;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11BlendState* state = nullptr;
        m_device->CreateBlendState(&bd, &state);
        return state;
    }

    ID3D11SamplerState* make_sampler(D3D11_FILTER filter, D3D11_TEXTURE_ADDRESS_MODE address)
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = filter;
        sd.AddressU = sd.AddressV = sd.AddressW = address;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11SamplerState* state = nullptr;
        m_device->CreateSamplerState(&sd, &state);
        return state;
    }

    // ---- the window over the game's -------------------------------------------------------------------------

    void place()
    {
        // the game's client area, where it is on the screen now
        RECT client;
        POINT origin{ 0, 0 };
        if (!game || !GetClientRect(game, &client) || !ClientToScreen(game, &origin))
            return;
        const RECT r{ origin.x, origin.y, origin.x + client.right, origin.y + client.bottom };
        const bool hidden = IsIconic(game) || !IsWindowVisible(game) || r.right <= r.left || r.bottom <= r.top;
        if (hidden)
        {
            if (IsWindowVisible(m_window))
                ShowWindow(m_window, SW_HIDE);
            return;
        }

        const u32 w = u32(r.right - r.left), h = u32(r.bottom - r.top);
        if (!m_chain || w != m_width || h != m_height)
            resize(w, h);

        // Right over the game's window and no higher, so another program the player went to stays over both.
        HWND above = GetWindow(game, GW_HWNDPREV);
        if (above == m_window)
            above = GetWindow(m_window, GW_HWNDPREV);
        if (above && (GetWindowLongW(above, GWL_EXSTYLE) & WS_EX_TOPMOST))
            above = HWND_TOP;
        const bool moved = memcmp(&r, &m_rect, sizeof(r)) != 0;
        if (moved || !IsWindowVisible(m_window) || GetWindow(game, GW_HWNDPREV) != m_window)
        {
            SetWindowPos(m_window, above ? above : HWND_TOP, r.left, r.top, int(w), int(h), SWP_NOACTIVATE | SWP_SHOWWINDOW);
            m_rect = r;
        }
    }

    void resize(u32 w, u32 h)
    {
        release(m_target);
        if (!m_chain)
        {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = w;
            desc.Height = h;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            if (FAILED(m_factory->CreateSwapChainForHwnd(m_device, m_window, &desc, nullptr, nullptr, &m_chain)))
                return;
            m_factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        }
        else if (FAILED(m_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
            return;
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(m_chain->GetBuffer(0, IID_PPV_ARGS(&back))))
            m_device->CreateRenderTargetView(back, nullptr, &m_target);
        release(back);
        m_width = w;
        m_height = h;
    }

    // ---- the layers ---------------------------------------------------------------------------------------

    float volume() const { return std::clamp(psSoundVFactor * psSoundVMusic, 0.f, 1.f); }

    std::unique_ptr<Source> open_source(pcstr texture)
    {
        string_path fn;
        switch (da_media_resolve(texture, fn))
        {
        case DaMediaKind::Video:
        {
            if (!m_mf)
                return nullptr;
            auto video = std::make_unique<VideoSource>(m_device);
            if (video->open(m_manager, fn, volume()))
                return video;
            Msg("! [loading] %s: Media Foundation cannot play this video", fn);
            return nullptr;
        }
        case DaMediaKind::Gif:
            if (CDaMediaSource* gif = CDaMediaSource::Open(DaMediaKind::Gif, fn))
                return std::make_unique<GifSource>(m_device, gif);
            return nullptr;
        case DaMediaKind::Theora:
        {
            auto* theora = xr_new<CTheoraSurface>();
            if (!theora->Load(fn))
            {
                xr_delete(theora);
                return nullptr;
            }
            theora->DecodeToColour();
            theora->Play(true, da_media_time());
            return std::make_unique<TheoraSource>(m_device, theora);
        }
        default: return nullptr;
        }
    }

    // a static's shader from its .s and .ps: the pixel shader, the blend, the other textures and the samplers
    std::unique_ptr<Program> build(pcstr shader)
    {
        string_path script;
        strconcat(script, "r3\\", shader, ".s");
        IReader* file = FS.r_open("$game_shaders$", script);
        if (!file)
        {
            Msg("! [loading] %s: no shaders\\r3\\%s.s", shader, shader);
            return nullptr;
        }
        const std::string text(static_cast<pcstr>(file->pointer()), file->length());
        FS.r_close(file);

        std::smatch m;
        if (!std::regex_search(text, m, std::regex(R"re(begin\s*\(\s*"[^"]*"\s*,\s*"([^"]+)")re")))
            return nullptr;
        const xr_string psName = m[1].str().c_str();
        auto program = std::make_unique<Program>();
        D3D11_BLEND src = D3D11_BLEND_SRC_ALPHA, dst = D3D11_BLEND_INV_SRC_ALPHA;
        if (std::regex_search(text, m, std::regex(R"re(blend\s*\(\s*(true|false)\s*,\s*blend\.(\w+)\s*,\s*blend\.(\w+))re")))
        {
            if (m[1] == "false")
                src = D3D11_BLEND_ONE, dst = D3D11_BLEND_ZERO;
            else
            {
                src = blend_factor(m[2].str().c_str(), src);
                dst = blend_factor(m[3].str().c_str(), dst);
            }
        }
        program->blend = make_blend(src, dst);

        string_path ps;
        strconcat(ps, "r3\\", psName.c_str(), ".ps");
        IReader* source = FS.r_open("$game_shaders$", ps);
        if (!source)
            return nullptr;
        const xr_string code(static_cast<pcstr>(source->pointer()), source->length());
        FS.r_close(source);
        ShaderIncludes includes;
        ID3DBlob* blob = compile(code.c_str(), "ps_5_0", ps, &includes);
        if (!blob || FAILED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &program->ps)))
        {
            release(blob);
            return nullptr;
        }

        // the bindings by name, as the game's shaders find theirs
        ID3D11ShaderReflection* reflection = nullptr;
        if (SUCCEEDED(m_reflect(blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&reflection))))
        {
            D3D11_SHADER_DESC sd;
            reflection->GetDesc(&sd);
            // the textures the .s names besides the static's: name -> texture
            std::unordered_map<std::string, std::string> named;
            static const std::regex texture_line(R"re(dx10texture\s*\(\s*"(\w+)"\s*,\s*"([^"]+)")re");
            for (auto it = std::sregex_iterator(text.begin(), text.end(), texture_line);
                 it != std::sregex_iterator(); ++it)
                named[(*it)[1].str()] = (*it)[2].str();
            for (UINT i = 0; i < sd.BoundResources; ++i)
            {
                D3D11_SHADER_INPUT_BIND_DESC bd;
                reflection->GetResourceBindingDesc(i, &bd);
                const std::string name = bd.Name;
                if (bd.Type == D3D_SIT_TEXTURE)
                {
                    if (name == "s_base")
                        program->baseSlot = bd.BindPoint;
                    else if (const auto t = named.find(name); t != named.end())
                    {
                        Program::Extra extra{ bd.BindPoint, {} };
                        load_dds(m_device, t->second.c_str(), extra.texture);
                        program->extras.push_back(extra);
                    }
                }
                else if (bd.Type == D3D_SIT_SAMPLER)
                {
                    // the game's samplers by name: smp_rtlinear clamps, smp_nofilter takes the nearest texel,
                    // smp_base and smp_linear repeat
                    const bool wrap = name == "smp_base" || name == "smp_linear";
                    program->samplers.push_back({ bd.BindPoint, wrap, name == "smp_nofilter" });
                }
                else if (bd.Type == D3D_SIT_CBUFFER && name == "$Globals")
                {
                    ID3D11ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByName("$Globals");
                    D3D11_SHADER_BUFFER_DESC cd;
                    if (cb && SUCCEEDED(cb->GetDesc(&cd)) && bd.BindPoint == 0)
                    {
                        program->globalsData.assign(cd.Size, 0);
                        for (UINT v = 0; v < cd.Variables; ++v)
                        {
                            D3D11_SHADER_VARIABLE_DESC vd;
                            cb->GetVariableByIndex(v)->GetDesc(&vd);
                            program->vars.push_back({ vd.Name, vd.StartOffset, vd.Size });
                        }
                        D3D11_BUFFER_DESC desc{};
                        desc.ByteWidth = cd.Size;
                        desc.Usage = D3D11_USAGE_DYNAMIC;
                        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                        m_device->CreateBuffer(&desc, nullptr, &program->globals);
                    }
                }
            }
            release(reflection);
        }
        release(blob);
        return program;
    }

    void prepare(const DaLoadingLayer& layer, LayerState& state)
    {
        if (state.revision == layer.revision)
            return;
        const bool sameItem = state.revision != ~0u;
        state.revision = layer.revision;
        const u32 w = u32(std::max(layer.rect.right - layer.rect.left, 0)), h = u32(std::max(layer.rect.bottom - layer.rect.top, 0));
        if (layer.pixels && w && h && layer.pixels->size() >= size_t(w) * h)
            make_texture(m_device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, layer.pixels->data(), w * 4, false, state.picture);
        if (layer.kind == DaLoadingLayer::Kind::Percent && layer.percents)
        {
            for (GpuTexture& p : state.percents)
                p.reset();
            state.percents.resize(layer.percents->size());
            for (size_t i = 0; i < layer.percents->size(); ++i)
                if (const DaPicture& p = (*layer.percents)[i]; p && p->size() >= size_t(w) * h)
                    make_texture(m_device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, p->data(), w * 4, false, state.percents[i]);
        }
        // a video, a GIF, a shader are made once per item: a new picture of the same item is its fallback
        if (sameItem)
            return;
        if (layer.kind == DaLoadingLayer::Kind::Media || layer.kind == DaLoadingLayer::Kind::Shader)
        {
            state.source = open_source(layer.texture.c_str());
            if (layer.kind == DaLoadingLayer::Kind::Shader)
            {
                state.program = build(layer.shader.c_str());
                if (!state.source)
                    load_dds(m_device, layer.texture.c_str(), state.image);
            }
        }
    }

    void draw_quad(const Fvector4& rect, const Fvector4& uv, const Fvector4& color, ID3D11ShaderResourceView* view,
        ID3D11PixelShader* ps = nullptr, ID3D11BlendState* blend = nullptr)
    {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_context->Map(m_quad, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        {
            QuadConstants q{ rect, uv, color, { float(m_width), float(m_height), 1.f / float(m_width), 1.f / float(m_height) } };
            CopyMemory(map.pData, &q, sizeof(q));
            m_context->Unmap(m_quad, 0);
        }
        m_context->VSSetConstantBuffers(0, 1, &m_quad);
        m_context->OMSetBlendState(blend ? blend : m_alpha, nullptr, 0xFFFFFFFF);
        if (!ps)
        {
            m_context->PSSetShader(m_ps, nullptr, 0);
            m_context->PSSetShaderResources(0, 1, &view);
            m_context->PSSetSamplers(0, 1, &m_clamp);
        }
        else
            m_context->PSSetShader(ps, nullptr, 0);
        m_context->Draw(4, 0);
    }

    void scissor(const Irect& r)
    {
        const D3D11_RECT rect{ std::max(r.left, 0), std::max(r.top, 0), std::max(r.right, 0), std::max(r.bottom, 0) };
        m_context->RSSetScissorRects(1, &rect);
    }

    void draw_layer(const DaLoadingLayer& layer, LayerState& state, u32 time)
    {
        const Fvector4 rect{ float(layer.rect.left), float(layer.rect.top), float(layer.rect.right), float(layer.rect.bottom) };
        const Fvector4 whole{ 0.f, 0.f, 1.f, 1.f };
        Irect screen;
        screen.set(0, 0, int(m_width), int(m_height));
        scissor(screen);
        switch (layer.kind)
        {
        case DaLoadingLayer::Kind::Picture:
            if (state.picture)
                draw_quad(rect, whole, { 1.f, 1.f, 1.f, 1.f }, state.picture.view);
            break;

        case DaLoadingLayer::Kind::Media:
            if (ID3D11ShaderResourceView* frame = state.source ? state.source->frame(m_context, time) : nullptr)
                draw_quad(rect, layer.uv, rgba(layer.color), frame);
            else if (state.picture)
                draw_quad(rect, whole, { 1.f, 1.f, 1.f, 1.f }, state.picture.view);
            break;

        case DaLoadingLayer::Kind::Shader:
        {
            ID3D11ShaderResourceView* base = state.source ? state.source->frame(m_context, time) : state.image.view;
            Program* p = state.program.get();
            if (!p)
            {
                if (state.picture)
                    draw_quad(rect, whole, { 1.f, 1.f, 1.f, 1.f }, state.picture.view);
                break;
            }
            // the constants the game gives the interface's shaders (da_media.h)
            const float seconds = float(time % 3600000) * 0.001f;
            p->set("xms_ui_time", { seconds, float(time - m_opened) * 0.001f, 1.f / 60.f, 0.f });
            p->set("xms_ui_rect", rect);
            p->set("xms_ui_uv", layer.uv);
            p->set("xms_ui_cursor", { 0.f, 0.f, 0.f, 0.f });
            p->set("xms_ui_progress", { m_eased, progress.load(std::memory_order_relaxed), 0.f, 0.f });
            p->set("screen_res", { float(m_width), float(m_height), 1.f / float(m_width), 1.f / float(m_height) });
            p->set("timers", { seconds, seconds * 10.f, seconds / 10.f, std::sin(seconds) });
            if (p->globals)
            {
                D3D11_MAPPED_SUBRESOURCE map;
                if (SUCCEEDED(m_context->Map(p->globals, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
                {
                    CopyMemory(map.pData, p->globalsData.data(), p->globalsData.size());
                    m_context->Unmap(p->globals, 0);
                }
                m_context->PSSetConstantBuffers(0, 1, &p->globals);
            }
            if (p->baseSlot != ~0u)
                m_context->PSSetShaderResources(p->baseSlot, 1, &base);
            for (Program::Extra& e : p->extras)
                m_context->PSSetShaderResources(e.slot, 1, &e.texture.view);
            for (const Program::Sampler& s : p->samplers)
                m_context->PSSetSamplers(s.slot, 1, s.point ? &m_point : (s.wrap ? &m_wrap : &m_clamp));
            draw_quad(rect, layer.uv, rgba(layer.color), base, p->ps, p->blend);
            break;
        }

        case DaLoadingLayer::Kind::Fill:
        {
            if (!state.picture)
                break;
            // cut the way CUIProgressBar::Draw cuts it, from the fill's corner, by the bar's size
            const float bw = layer.bar.width(), bh = layer.bar.height();
            const bool across = layer.orient == 0 || layer.orient == 2 || layer.orient == 4;
            const float len = (across ? bw : bh) * m_eased;
            if (len <= 0.f)
                break;
            Frect cut;
            switch (layer.orient)
            {
            case 1: cut.set(0, bh - len, bw, bh); break;
            case 2: cut.set(bw - len * 1.01f, 0, bw, bh); break;
            case 3: cut.set(0, 0, bw, len); break;
            case 4: cut.set(bw / 2.f - len, 0, bw / 2.f + len, bh); break;
            case 5: cut.set(0, bh / 2.f - len, bw, bh / 2.f + len); break;
            default: cut.set(0, 0, len, bh); break;
            }
            cut.add(rect.x, rect.y);
            Irect clip;
            clip.set(iFloor(cut.x1 + 0.5f), iFloor(cut.y1 + 0.5f), iFloor(cut.x2 + 0.5f), iFloor(cut.y2 + 0.5f));
            scissor(clip);
            Fvector4 tint{ 1.f, 1.f, 1.f, 1.f };
            if (layer.tinted)
            {
                Fcolor c;
                if (layer.tint_middle)
                    c.lerp(layer.tint_min, layer.tint_mid, layer.tint_max, m_eased);
                else
                    c.lerp(layer.tint_min, layer.tint_max, m_eased);
                tint = { c.r, c.g, c.b, c.a };
            }
            draw_quad(rect, whole, tint, state.picture.view);
            break;
        }

        case DaLoadingLayer::Kind::Percent:
        {
            const size_t i = size_t(std::clamp(iFloor(m_eased * 100.f + 0.5f), 0, 100));
            if (i < state.percents.size() && state.percents[i])
                draw_quad(rect, whole, { 1.f, 1.f, 1.f, 1.f }, state.percents[i].view);
            break;
        }
        }
    }

    void render(u32 time)
    {
        if (!m_target)
            return;
        // progress eases after each step: half the way in about a tenth of a second
        const float dt = std::min(float(time - m_easedAt) * 0.001f, 0.25f);
        m_easedAt = time;
        m_eased += (progress.load(std::memory_order_relaxed) - m_eased) * (1.f - std::exp(-dt * 7.f));

        const float black[4] = { 0.f, 0.f, 0.f, 1.f };
        m_context->ClearRenderTargetView(m_target, black);
        m_context->OMSetRenderTargets(1, &m_target, nullptr);
        const D3D11_VIEWPORT vp{ 0.f, 0.f, float(m_width), float(m_height), 0.f, 1.f };
        m_context->RSSetViewports(1, &vp);
        m_context->RSSetState(m_raster);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_context->IASetInputLayout(nullptr);
        m_context->VSSetShader(m_vs, nullptr, 0);

        // The game laid out its screen at its own size; this window follows the client area, which is that size
        // but for a moment of a resize.
        for (const DaLoadingLayer& layer : m_scene)
            draw_layer(layer, m_layers[layer.id], time);

        const float vol = volume();
        for (auto& [id, state] : m_layers)
            if (state.source)
                state.source->volume(vol);

        capture(time);
        const HRESULT hr = m_chain->Present(1, 0);
        if (hr == DXGI_STATUS_OCCLUDED)
            std::this_thread::sleep_for(std::chrono::milliseconds(16));

        // the proof of it in the log: the longest wait between two frames of a loading, and how many waited long
        if (m_frames++)
        {
            m_longest = std::max(m_longest, time - m_lastFrame);
            m_slow += time - m_lastFrame > 50;
        }
        m_lastFrame = time;
    }

    void pump()
    {
        MSG msg;
        while (PeekMessageW(&msg, m_window, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void run()
    {
        const bool ok = init();
        {
            std::lock_guard guard(lock);
            failed = !ok;
        }
        wake.notify_all();
        if (!ok)
        {
            Msg("! [loading] the loading screen's own device is not available: the game draws it between its steps");
            shutdown();
            return;
        }

        while (true)
        {
            bool shown;
            {
                std::unique_lock guard(lock);
                wake.wait_for(guard, std::chrono::milliseconds(m_shown || m_fadeFrom ? 0 : 50),
                    [&] { return quit || want != m_shown || sceneVersion != m_version; });
                if (quit)
                    break;
                if (sceneVersion != m_version)
                {
                    m_scene = scene;
                    m_version = sceneVersion;
                }
                shown = want;
            }
            pump();
            const u32 now = da_media_time();

            if (shown && !m_shown)
            {
                // a loading of its own: nothing of the one before
                m_shown = true;
                m_fadeFrom = 0;
                m_layers.clear();
                m_opened = m_easedAt = now;
                m_eased = progress.load(std::memory_order_relaxed);
                m_frames = m_longest = m_slow = 0;
            }
            else if (!shown && m_shown)
            {
                m_shown = false;
                ShowWindow(m_window, SW_HIDE);
                Msg("* [loading] its own thread drew %u frame(s); the longest wait between two %u ms, %u over 50 ms", m_frames,
                    m_longest, m_slow);
                m_fadeFrom = now;
            }

            if (m_shown)
            {
                for (const DaLoadingLayer& layer : m_scene)
                    prepare(layer, m_layers[layer.id]);
                place();
                if (IsWindowVisible(m_window))
                    render(now);
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            else if (m_fadeFrom)
            {
                // the sound of a video goes out in a quarter of a second, then the loading's layers go
                const float k = 1.f - std::min(float(now - m_fadeFrom) / 250.f, 1.f);
                for (auto& [id, state] : m_layers)
                    if (state.source)
                        state.source->volume(volume() * k);
                if (k <= 0.f)
                {
                    m_fadeFrom = 0;
                    m_layers.clear();
                    m_scene.clear();
                }
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        shutdown();
    }
};

std::mutex presenter_lock;
Presenter* presenter = nullptr;
bool presenter_failed = false;

Presenter* get_presenter()
{
    std::lock_guard guard(presenter_lock);
    if (!presenter && !presenter_failed)
    {
        presenter = xr_new<Presenter>();
        presenter->game = static_cast<HWND>(Device.GetApplicationWindowHandle());
        if (!presenter->start())
        {
            presenter->stop();
            xr_delete(presenter);
            presenter_failed = true;
        }
    }
    return presenter;
}

u32 scene_version = 0;
} // namespace

bool da_loading_presenter_usable()
{
    return psLoadingScreenAsync && !GEnv.isDedicatedServer && psDeviceMode.WindowStyle != rsFullscreen && !presenter_failed &&
        Device.GetApplicationWindowHandle();
}

void da_loading_presenter_publish(const DaLoadingScene& scene)
{
    Presenter* p = get_presenter();
    if (!p)
        return;
    {
        std::lock_guard guard(p->lock);
        p->scene = scene;
        p->sceneVersion = ++scene_version;
        p->want = true;
        if (psLoadingScreenCapture && !p->captureDir[0])
        {
            string64 stamp;
            timestamp(stamp);
            string_path folder;
            xr_sprintf(folder, "loading_%s" DELIMITER, stamp);
            FS.update_path(p->captureDir, "$screenshots$", folder);
            VerifyPath(p->captureDir);
        }
    }
    p->wake.notify_all();
}

void da_loading_presenter_progress(float progress)
{
    if (Presenter* p = presenter)
        p->progress.store(std::clamp(progress, 0.f, 1.f), std::memory_order_relaxed);
}

void da_loading_presenter_hide()
{
    Presenter* p = presenter;
    if (!p)
        return;
    {
        std::lock_guard guard(p->lock);
        if (!p->want)
            return;
        p->want = false;
    }
    p->wake.notify_all();
}

bool da_loading_presenter_shown()
{
    Presenter* p = presenter;
    if (!p)
        return false;
    std::lock_guard guard(p->lock);
    return p->want;
}

void da_loading_presenter_shutdown()
{
    std::lock_guard guard(presenter_lock);
    if (presenter)
    {
        presenter->stop();
        xr_delete(presenter);
    }
}
#else
bool da_loading_presenter_usable() { return false; }
void da_loading_presenter_publish(const DaLoadingScene&) {}
void da_loading_presenter_progress(float) {}
void da_loading_presenter_hide() {}
bool da_loading_presenter_shown() { return false; }
void da_loading_presenter_shutdown() {}
#endif
