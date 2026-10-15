// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt
//
// Private GPU helpers of da_ground.cpp and da_snow.cpp (docs/dead-air/SNOW.md): named textures written
// through UAVs, upload buffers, single-element compute shaders and their dispatch. Not a public header.
#pragma once

#include <functional>
#include <initializer_list>
#include <utility>

namespace xray::render::RENDER_NAMESPACE
{
IReader* open_shader(pcstr shader); // ResourceManager.cpp

namespace da_snow_gpu
{
// Old content without the snow shaders keeps the feature off instead of failing a compile.
inline bool shader_exists(pcstr file)
{
    IReader* reader = open_shader(file);
    const bool exists = !!reader;
    FS.r_close(reader);
    return exists;
}

template <typename T>
void release(T*& object)
{
    if (object)
        object->Release();
    object = nullptr;
}

// A 2D texture under a $user$ name, read by name in the blenders and written through its own UAV. The
// named object is kept for the renderer's lifetime once made (compiled passes hold pointers to it);
// only its surface is replaced, by a 1 x 1 placeholder when the real one goes.
struct Texture
{
    ref_texture named;
    ID3D11Texture2D* surface = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    u32 width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    bool live() const { return surface && width > 1; }
    ID3D11ShaderResourceView* srv() const { return named ? named->get_SRView() : nullptr; }

    bool create(pcstr name, u32 w, u32 h, DXGI_FORMAT fmt, bool with_uav, const void* data = nullptr, u32 pitch = 0)
    {
        drop();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = with_uav || !data ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (with_uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
        D3D11_SUBRESOURCE_DATA init{data, pitch, 0};
        if (FAILED(HW.pDevice->CreateTexture2D(&desc, data ? &init : nullptr, &surface)) || !surface)
        {
            surface = nullptr;
            return false;
        }
        if (with_uav && FAILED(HW.pDevice->CreateUnorderedAccessView(surface, nullptr, &uav)))
        {
            uav = nullptr;
            release(surface);
            return false;
        }
        if (!named)
            named = RImplementation.Resources->_CreateTexture(name);
        named->surface_set(surface);
        width = w;
        height = h;
        format = fmt;
        return true;
    }

    // A zero 1 x 1 surface: the name resolves and every read of it is "no data".
    void placeholder(pcstr name, DXGI_FORMAT fmt)
    {
        const u32 zero[4]{};
        create(name, 1, 1, fmt, false, zero, sizeof(zero));
        width = 1;
        height = 1;
    }

    void drop()
    {
        release(uav);
        release(surface);
        width = height = 0;
    }

    // Renderer shutdown: nothing is kept.
    void destroy()
    {
        if (named)
            named->surface_set(nullptr);
        drop();
        named = nullptr;
    }
};

// A DEFAULT buffer filled with UpdateSubresource (the DirectX 12 layer has no SRVs on DYNAMIC buffers).
struct Buffer
{
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    u32 capacity = 0; // elements
    u32 stride = 0;   // 0 = raw (ByteAddressBuffer, 4-byte words)

    bool reserve(u32 elements, u32 element_stride, bool raw)
    {
        if (buffer && elements <= capacity && element_stride == stride)
            return true;
        destroy();
        const u32 count = std::max(elements, 64u);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = count * element_stride;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = raw ? 0 : element_stride;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        if (raw)
        {
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
            view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
            view.BufferEx.NumElements = desc.ByteWidth / 4;
        }
        else
        {
            view.Format = DXGI_FORMAT_UNKNOWN;
            view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            view.Buffer.NumElements = count;
        }
        if (FAILED(HW.pDevice->CreateBuffer(&desc, nullptr, &buffer)) || !buffer ||
            FAILED(HW.pDevice->CreateShaderResourceView(buffer, &view, &srv)))
        {
            destroy();
            return false;
        }
        capacity = count;
        stride = element_stride;
        return true;
    }

    void upload(CBackend& cmd, const void* data, u32 elements) const
    {
        if (!buffer || !elements)
            return;
        const D3D11_BOX box{0, 0, 0, elements * stride, 1, 1};
        HW.get_context(cmd.context_id)->UpdateSubresource(buffer, 0, &box, data, 0, 0);
    }

    void destroy()
    {
        release(srv);
        release(buffer);
        capacity = 0;
        stride = 0;
    }
};

// One compute pass: a CS and the textures its blender binds by name.
class CBlender_compute : public IBlender
{
    pcstr m_cs = nullptr;
    xr_vector<std::pair<pcstr, pcstr>> m_textures;

public:
    void setup(pcstr cs, std::initializer_list<std::pair<pcstr, pcstr>> textures)
    {
        m_cs = cs;
        m_textures.assign(textures.begin(), textures.end());
    }
    LPCSTR getComment() override { return "INTERNAL: snow / ground compute"; }
    BOOL canBeDetailed() override { return FALSE; }
    BOOL canBeLMAPped() override { return FALSE; }
    void Compile(CBlender_Compile& C) override
    {
        IBlender::Compile(C);
        if (C.iElement > 0)
            return;
        C.r_ComputePass(m_cs);
        for (const auto& [name, texture] : m_textures)
            C.r_dx11Texture(name, texture);
        C.r_End();
    }
};

struct Compute
{
    CBlender_compute blender;
    ref_shader shader;

    // The shader's file must exist; false keeps the feature off.
    bool create(pcstr cs, std::initializer_list<std::pair<pcstr, pcstr>> textures)
    {
        string_path file;
        strconcat(file, cs, ".cs");
        if (!shader_exists(file))
            return false;
        blender.setup(cs, textures);
        string_path name;
        strconcat(name, "r2" DELIMITER, cs);
        shader.create(&blender, name);
        return ready();
    }
    bool ready() const
    {
        return shader && shader->E[0] && !shader->E[0]->passes.empty() && shader->E[0]->passes[0]->cs &&
            shader->E[0]->passes[0]->cs->sh;
    }
    void destroy() { shader = nullptr; }
};

// A dispatch with its SRVs at fixed registers and its UAVs from u0, everything unbound again after.
inline void dispatch(CBackend& cmd, const Compute& pass, u32 gx, u32 gy, u32 gz,
    std::initializer_list<std::pair<u32, ID3D11ShaderResourceView*>> srvs,
    std::initializer_list<ID3D11UnorderedAccessView*> uavs, const std::function<void()>& constants)
{
    if (!gx || !gy || !gz)
        return;
    SPass& P = *pass.shader->E[0]->passes[0];
    cmd.set_States(P.state);
    cmd.set_CS(P.cs);
    cmd.set_Constants(P.constants);
    if (P.T)
        cmd.set_Textures(P.T);
    constants();
    for (const auto& [slot, srv] : srvs)
        cmd.SRVSManager.SetCSResource(slot, srv);
    ID3D11DeviceContext* context = HW.get_context(cmd.context_id);
    ID3D11UnorderedAccessView* bound[4]{};
    const UINT initial[4] = {~0u, ~0u, ~0u, ~0u};
    u32 count = 0;
    for (ID3D11UnorderedAccessView* uav : uavs)
        bound[count++] = uav;
    context->CSSetUnorderedAccessViews(0, count, bound, initial);
    cmd.Compute(gx, gy, gz);
    ID3D11UnorderedAccessView* none[4]{};
    context->CSSetUnorderedAccessViews(0, count, none, initial);
    for (const auto& [slot, srv] : srvs)
        cmd.SRVSManager.SetCSResource(slot, nullptr);
    cmd.clear_CS_resources();
    cmd.SRVSManager.Apply(cmd.context_id);
}

inline int floor_div(int v, int d)
{
    return v >= 0 ? v / d : -((-v + d - 1) / d);
}

// A rectangle of world texels, [x0, x1) x [z0, z1).
struct Rect
{
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    bool empty() const { return x0 >= x1 || z0 >= z1; }
    int width() const { return x1 - x0; }
    int height() const { return z1 - z0; }
};

// The rectangles a toroidal window of n texels newly covers when its first texel moves from (ox, oz)
// to (nx, nz): one strip per axis, or the whole window after a jump.
inline u32 window_strips(int ox, int oz, int nx, int nz, int n, bool valid, Rect out[2])
{
    if (!valid || std::abs(nx - ox) >= n || std::abs(nz - oz) >= n)
    {
        out[0] = {nx, nz, nx + n, nz + n};
        return 1;
    }
    u32 count = 0;
    if (nx > ox)
        out[count++] = {ox + n, nz, nx + n, nz + n};
    else if (nx < ox)
        out[count++] = {nx, nz, ox, nz + n};
    if (nz > oz)
        out[count++] = {nx, oz + n, nx + n, nz + n};
    else if (nz < oz)
        out[count++] = {nx, nz, nx + n, oz};
    return count;
}
} // namespace da_snow_gpu
} // namespace xray::render::RENDER_NAMESPACE
