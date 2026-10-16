#include "stdafx.h"

#include "dxImGuiRender.h"

#if defined(XR_VULKAN)
#include "Layers/xrRenderPC_R4/da_dxc.h"
#elif defined(USE_DX11)
#include <backends/imgui_impl_dx11.h>
#elif defined(USE_OGL)
#include <backends/imgui_impl_opengl3.h>
#endif

namespace xray::render::RENDER_NAMESPACE
{
#if defined(XR_VULKAN)
// Dear ImGui on the Vulkan module. ImGui's DX11 backend (in xrEngine) compiles its shaders with FXC, and
// Vulkan runs no DXBC: this is the same renderer over the same D3D11 interfaces, its shaders through DXC.
namespace
{
constexpr char imgui_vs[] = R"(
cbuffer imgui : register(b0) { float4x4 projection; };
struct v2p { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
v2p main(float2 pos : POSITION, float4 col : COLOR0, float2 uv : TEXCOORD0)
{
    v2p o;
    o.pos = mul(float4(pos, 0.f, 1.f), projection);
    o.col = col;
    o.uv = uv;
    return o;
}
)";
constexpr char imgui_ps[] = R"(
struct v2p { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
SamplerState s_imgui : register(s0);
Texture2D t_imgui : register(t0);
float4 main(v2p i) : SV_Target { return i.col * t_imgui.Sample(s_imgui, i.uv); }
)";

struct ImGuiVkTexture
{
    ID3D11Texture2D* texture{};
    ID3D11ShaderResourceView* view{};
};

struct ImGuiVk
{
    ID3D11VertexShader* vs{};
    ID3D11PixelShader* ps{};
    ID3D11InputLayout* layout{};
    ID3D11Buffer* constants{};
    ID3D11Buffer* vertices{};
    ID3D11Buffer* indices{};
    int vertex_capacity{};
    int index_capacity{};
    ID3D11BlendState* blend{};
    ID3D11RasterizerState* raster{};
    ID3D11DepthStencilState* depth{};
    ID3D11SamplerState* linear{};
    ID3D11SamplerState* nearest{};
    ID3D11DeviceContext* drawing{}; // the context while Render runs (the sampler callbacks)
} g_imgui;

template <typename T>
void imgui_release(T*& object)
{
    if (object)
        object->Release();
    object = nullptr;
}

void imgui_reset_state(const ImDrawList*, const ImDrawCmd*) {} // an identifier: Render re-applies its state
void imgui_sampler_linear(const ImDrawList*, const ImDrawCmd*) { g_imgui.drawing->PSSetSamplers(0, 1, &g_imgui.linear); }
void imgui_sampler_nearest(const ImDrawList*, const ImDrawCmd*) { g_imgui.drawing->PSSetSamplers(0, 1, &g_imgui.nearest); }

bool imgui_shader(pcstr name, pcstr source, pcstr target, xr_vector<u8>& code)
{
    DaDxcNative request;
    request.path = name;
    request.source = source;
    request.entry = "main";
    request.target = target;
    return da_dxc_compile_native(request, code);
}

bool imgui_create_objects()
{
    ID3D11Device* device = HW.pDevice;
    xr_vector<u8> vs, ps;
    if (!imgui_shader("imgui_vs.hlsl", imgui_vs, "vs_6_0", vs) || !imgui_shader("imgui_ps.hlsl", imgui_ps, "ps_6_0", ps) ||
        FAILED(device->CreateVertexShader(vs.data(), vs.size(), nullptr, &g_imgui.vs)) ||
        FAILED(device->CreatePixelShader(ps.data(), ps.size(), nullptr, &g_imgui.ps)))
    {
        Msg("! [imgui] the Vulkan renderer's ImGui shaders could not be made");
        return false;
    }
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(ImDrawVert, pos)), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, UINT(offsetof(ImDrawVert, uv)), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, UINT(offsetof(ImDrawVert, col)), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(elements, UINT(std::size(elements)), vs.data(), vs.size(), &g_imgui.layout);

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(float) * 16;
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&cb, nullptr, &g_imgui.constants);

    D3D11_BLEND_DESC blend{};
    D3D11_RENDER_TARGET_BLEND_DESC& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&blend, &g_imgui.blend);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.ScissorEnable = TRUE;
    raster.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&raster, &g_imgui.raster);

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth.FrontFace.StencilFailOp = depth.FrontFace.StencilDepthFailOp = depth.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depth.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    depth.BackFace = depth.FrontFace;
    device->CreateDepthStencilState(&depth, &g_imgui.depth);

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    device->CreateSamplerState(&sampler, &g_imgui.linear);
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    device->CreateSamplerState(&sampler, &g_imgui.nearest);
    return g_imgui.layout && g_imgui.constants && g_imgui.blend && g_imgui.raster && g_imgui.depth && g_imgui.linear &&
        g_imgui.nearest;
}

void imgui_destroy_texture(ImTextureData* tex)
{
    if (auto* backend = static_cast<ImGuiVkTexture*>(tex->BackendUserData))
    {
        imgui_release(backend->view);
        imgui_release(backend->texture);
        IM_DELETE(backend);
        tex->SetTexID(ImTextureID_Invalid);
        tex->BackendUserData = nullptr;
    }
    tex->SetStatus(ImTextureStatus_Destroyed);
}

void imgui_destroy_objects()
{
    if (ImGui::GetCurrentContext())
    {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        {
            if (tex->RefCount == 1)
                imgui_destroy_texture(tex);
        }
    }
    imgui_release(g_imgui.nearest);
    imgui_release(g_imgui.linear);
    imgui_release(g_imgui.depth);
    imgui_release(g_imgui.raster);
    imgui_release(g_imgui.blend);
    imgui_release(g_imgui.indices);
    imgui_release(g_imgui.vertices);
    imgui_release(g_imgui.constants);
    imgui_release(g_imgui.layout);
    imgui_release(g_imgui.ps);
    imgui_release(g_imgui.vs);
    g_imgui.vertex_capacity = g_imgui.index_capacity = 0;
}

// ImGui 1.92's texture requests: the font atlas grows and changes while the game runs.
void imgui_update_texture(ImTextureData* tex)
{
    if (tex->Status == ImTextureStatus_WantCreate)
    {
        auto* backend = IM_NEW(ImGuiVkTexture)();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(tex->Width);
        desc.Height = UINT(tex->Height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA data{tex->GetPixels(), desc.Width * 4, 0};
        HW.pDevice->CreateTexture2D(&desc, &data, &backend->texture);
        if (backend->texture)
            HW.pDevice->CreateShaderResourceView(backend->texture, nullptr, &backend->view);
        tex->SetTexID(ImTextureID(intptr_t(backend->view)));
        tex->BackendUserData = backend;
        tex->SetStatus(ImTextureStatus_OK);
    }
    else if (tex->Status == ImTextureStatus_WantUpdates)
    {
        auto* backend = static_cast<ImGuiVkTexture*>(tex->BackendUserData);
        ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
        for (const ImTextureRect& r : tex->Updates)
        {
            const D3D11_BOX box{UINT(r.x), UINT(r.y), 0, UINT(r.x + r.w), UINT(r.y + r.h), 1};
            if (backend && backend->texture)
                context->UpdateSubresource(backend->texture, 0, &box, tex->GetPixelsAt(r.x, r.y), UINT(tex->GetPitch()), 0);
        }
        tex->SetStatus(ImTextureStatus_OK);
    }
    if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
        imgui_destroy_texture(tex);
}

void imgui_setup_state(const ImDrawData* data, ID3D11DeviceContext* context)
{
    const D3D11_VIEWPORT viewport{0.f, 0.f, data->DisplaySize.x * data->FramebufferScale.x,
        data->DisplaySize.y * data->FramebufferScale.y, 0.f, 1.f};
    context->RSSetViewports(1, &viewport);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(g_imgui.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        const float L = data->DisplayPos.x, R = data->DisplayPos.x + data->DisplaySize.x;
        const float T = data->DisplayPos.y, B = data->DisplayPos.y + data->DisplaySize.y;
        const float projection[4][4] = {
            {2.f / (R - L), 0.f, 0.f, 0.f},
            {0.f, 2.f / (T - B), 0.f, 0.f},
            {0.f, 0.f, 0.5f, 0.f},
            {(R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.f},
        };
        std::memcpy(mapped.pData, projection, sizeof(projection));
        context->Unmap(g_imgui.constants, 0);
    }
    const UINT stride = sizeof(ImDrawVert), offset = 0;
    context->IASetInputLayout(g_imgui.layout);
    context->IASetVertexBuffers(0, 1, &g_imgui.vertices, &stride, &offset);
    context->IASetIndexBuffer(g_imgui.indices, sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_imgui.vs, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &g_imgui.constants);
    context->PSSetShader(g_imgui.ps, nullptr, 0);
    context->PSSetSamplers(0, 1, &g_imgui.linear);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    const float factor[4]{};
    context->OMSetBlendState(g_imgui.blend, factor, 0xffffffff);
    context->OMSetDepthStencilState(g_imgui.depth, 0);
    context->RSSetState(g_imgui.raster);
}

bool imgui_grow(ID3D11Buffer*& buffer, int& capacity, int needed, int spare, UINT stride, UINT bind)
{
    if (buffer && capacity >= needed)
        return true;
    imgui_release(buffer);
    capacity = needed + spare;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = UINT(capacity) * stride;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = bind;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(HW.pDevice->CreateBuffer(&desc, nullptr, &buffer));
}

void imgui_render(ImDrawData* data)
{
    if (data->DisplaySize.x <= 0.f || data->DisplaySize.y <= 0.f || !g_imgui.vs)
        return;
    if (data->Textures)
    {
        for (ImTextureData* tex : *data->Textures)
        {
            if (tex->Status != ImTextureStatus_OK)
                imgui_update_texture(tex);
        }
    }
    if (!data->TotalVtxCount || !data->TotalIdxCount)
        return;
    if (!imgui_grow(g_imgui.vertices, g_imgui.vertex_capacity, data->TotalVtxCount, 5000, sizeof(ImDrawVert), D3D11_BIND_VERTEX_BUFFER) ||
        !imgui_grow(g_imgui.indices, g_imgui.index_capacity, data->TotalIdxCount, 10000, sizeof(ImDrawIdx), D3D11_BIND_INDEX_BUFFER))
        return;
    ID3D11DeviceContext* context = HW.get_context(CHW::IMM_CTX_ID);
    D3D11_MAPPED_SUBRESOURCE vtx{}, idx{};
    if (FAILED(context->Map(g_imgui.vertices, 0, D3D11_MAP_WRITE_DISCARD, 0, &vtx)))
        return;
    if (FAILED(context->Map(g_imgui.indices, 0, D3D11_MAP_WRITE_DISCARD, 0, &idx)))
    {
        context->Unmap(g_imgui.vertices, 0);
        return;
    }
    auto* vtx_dst = static_cast<ImDrawVert*>(vtx.pData);
    auto* idx_dst = static_cast<ImDrawIdx*>(idx.pData);
    for (const ImDrawList* list : data->CmdLists)
    {
        std::memcpy(vtx_dst, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
        std::memcpy(idx_dst, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
        vtx_dst += list->VtxBuffer.Size;
        idx_dst += list->IdxBuffer.Size;
    }
    context->Unmap(g_imgui.vertices, 0);
    context->Unmap(g_imgui.indices, 0);

    // The engine's state comes back as it was: its backend caches what it set on the context.
    struct Saved
    {
        UINT scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        ID3D11RasterizerState* raster{};
        ID3D11BlendState* blend{};
        FLOAT factor[4]{};
        UINT mask{};
        UINT stencil_ref{};
        ID3D11DepthStencilState* depth{};
        ID3D11ShaderResourceView* srv{};
        ID3D11SamplerState* sampler{};
        ID3D11PixelShader* ps{};
        ID3D11VertexShader* vs{};
        ID3D11GeometryShader* gs{};
        ID3D11HullShader* hs{};
        ID3D11DomainShader* ds{};
        ID3D11Buffer* vs_cb{};
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        ID3D11Buffer* ib{};
        DXGI_FORMAT ib_format{};
        UINT ib_offset{};
        ID3D11Buffer* vb{};
        UINT vb_stride{}, vb_offset{};
        ID3D11InputLayout* layout{};
    } old;
    context->RSGetScissorRects(&old.scissor_count, old.scissors);
    context->RSGetViewports(&old.viewport_count, old.viewports);
    context->RSGetState(&old.raster);
    context->OMGetBlendState(&old.blend, old.factor, &old.mask);
    context->OMGetDepthStencilState(&old.depth, &old.stencil_ref);
    context->PSGetShaderResources(0, 1, &old.srv);
    context->PSGetSamplers(0, 1, &old.sampler);
    context->PSGetShader(&old.ps, nullptr, nullptr);
    context->VSGetShader(&old.vs, nullptr, nullptr);
    context->GSGetShader(&old.gs, nullptr, nullptr);
    context->HSGetShader(&old.hs, nullptr, nullptr);
    context->DSGetShader(&old.ds, nullptr, nullptr);
    context->VSGetConstantBuffers(0, 1, &old.vs_cb);
    context->IAGetPrimitiveTopology(&old.topology);
    context->IAGetIndexBuffer(&old.ib, &old.ib_format, &old.ib_offset);
    context->IAGetVertexBuffers(0, 1, &old.vb, &old.vb_stride, &old.vb_offset);
    context->IAGetInputLayout(&old.layout);

    imgui_setup_state(data, context);
    g_imgui.drawing = context;
    int idx_offset = 0, vtx_offset = 0;
    const ImVec2 clip_off = data->DisplayPos, clip_scale = data->FramebufferScale;
    for (const ImDrawList* list : data->CmdLists)
    {
        for (const ImDrawCmd& cmd : list->CmdBuffer)
        {
            if (cmd.UserCallback)
            {
                if (cmd.UserCallback == imgui_reset_state)
                    imgui_setup_state(data, context);
                else
                    cmd.UserCallback(list, &cmd);
                continue;
            }
            const ImVec2 lo((cmd.ClipRect.x - clip_off.x) * clip_scale.x, (cmd.ClipRect.y - clip_off.y) * clip_scale.y);
            const ImVec2 hi((cmd.ClipRect.z - clip_off.x) * clip_scale.x, (cmd.ClipRect.w - clip_off.y) * clip_scale.y);
            if (hi.x <= lo.x || hi.y <= lo.y)
                continue;
            const D3D11_RECT scissor{LONG(lo.x), LONG(lo.y), LONG(hi.x), LONG(hi.y)};
            context->RSSetScissorRects(1, &scissor);
            auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(cmd.GetTexID());
            context->PSSetShaderResources(0, 1, &srv);
            context->DrawIndexed(cmd.ElemCount, cmd.IdxOffset + idx_offset, INT(cmd.VtxOffset + vtx_offset));
        }
        idx_offset += list->IdxBuffer.Size;
        vtx_offset += list->VtxBuffer.Size;
    }
    g_imgui.drawing = nullptr;

    context->RSSetScissorRects(old.scissor_count, old.scissors);
    context->RSSetViewports(old.viewport_count, old.viewports);
    context->RSSetState(old.raster);
    context->OMSetBlendState(old.blend, old.factor, old.mask);
    context->OMSetDepthStencilState(old.depth, old.stencil_ref);
    context->PSSetShaderResources(0, 1, &old.srv);
    context->PSSetSamplers(0, 1, &old.sampler);
    context->PSSetShader(old.ps, nullptr, 0);
    context->VSSetShader(old.vs, nullptr, 0);
    context->GSSetShader(old.gs, nullptr, 0);
    context->HSSetShader(old.hs, nullptr, 0);
    context->DSSetShader(old.ds, nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &old.vs_cb);
    context->IASetPrimitiveTopology(old.topology);
    context->IASetIndexBuffer(old.ib, old.ib_format, old.ib_offset);
    context->IASetVertexBuffers(0, 1, &old.vb, &old.vb_stride, &old.vb_offset);
    context->IASetInputLayout(old.layout);
    imgui_release(old.raster);
    imgui_release(old.blend);
    imgui_release(old.depth);
    imgui_release(old.srv);
    imgui_release(old.sampler);
    imgui_release(old.ps);
    imgui_release(old.vs);
    imgui_release(old.gs);
    imgui_release(old.hs);
    imgui_release(old.ds);
    imgui_release(old.vs_cb);
    imgui_release(old.ib);
    imgui_release(old.vb);
    imgui_release(old.layout);
}
} // namespace
#endif

void dxImGuiRender::Copy(IImGuiRender& _in)
{
    *this = *dynamic_cast<dxImGuiRender*>(&_in);
}

void dxImGuiRender::SetState(ImDrawData* data)
{
    RCache.SetViewport({ 0.f, 0.f, data->DisplaySize.x, data->DisplaySize.y, 0.f, 1.f });

    // Setup shader and vertex buffers
    /*unsigned int stride = sizeof(ImDrawVert);
    unsigned int offset = 0;
    ctx->IASetInputLayout(bd->pInputLayout);
    ctx->IASetVertexBuffers(0, 1, &bd->pVB, &stride, &offset);
    ctx->IASetIndexBuffer(bd->pIB, sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(bd->pVertexShader, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &bd->pVertexConstantBuffer);
    ctx->PSSetShader(bd->pPixelShader, nullptr, 0);
    ctx->PSSetSamplers(0, 1, &bd->pFontSampler);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0); // In theory we should backup and restore this as well.. very infrequently used..
    ctx->DSSetShader(nullptr, nullptr, 0); // In theory we should backup and restore this as well.. very infrequently used..
    ctx->CSSetShader(nullptr, nullptr, 0); // In theory we should backup and restore this as well.. very infrequently used..

    // Setup blend state
    const float blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
    ctx->OMSetBlendState(bd->pBlendState, blend_factor, 0xffffffff);
    ctx->OMSetDepthStencilState(bd->pDepthStencilState, 0);
    ctx->RSSetState(bd->pRasterizerState);*/
}

void dxImGuiRender::Frame()
{
#if defined(XR_VULKAN)
    if (!g_imgui.vs)
        imgui_create_objects();
#elif defined(USE_DX11)
    ImGui_ImplDX11_NewFrame();
#elif defined(USE_OGL)
    ImGui_ImplOpenGL3_NewFrame();
#endif
}

void dxImGuiRender::Render(ImDrawData* data)
{
#if defined(XR_VULKAN)
    imgui_render(data);
#elif defined(USE_DX11)
    ImGui_ImplDX11_RenderDrawData(data);
#elif defined(USE_OGL)
    ImGui_ImplOpenGL3_RenderDrawData(data);
#endif
}

void dxImGuiRender::OnDeviceCreate(ImGuiContext* context)
{
    ImGui::SetAllocatorFunctions(
        [](size_t size, void* /*user_data*/)
        {
            return xr_malloc(size);
        },
        [](void* ptr, void* /*user_data*/)
        {
            xr_free(ptr);
        }
    );
    ImGui::SetCurrentContext(context);

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "xrRender";

#if defined(XR_VULKAN)
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = platform.Renderer_TextureMaxHeight = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    platform.DrawCallback_ResetRenderState = imgui_reset_state;
    platform.DrawCallback_SetSamplerLinear = imgui_sampler_linear;
    platform.DrawCallback_SetSamplerNearest = imgui_sampler_nearest;
#elif defined(USE_DX11)
    ImGui_ImplDX11_Init(HW.pDevice, HW.get_context(CHW::IMM_CTX_ID));
#elif defined(USE_OGL)
    ImGui_ImplOpenGL3_Init();
#endif
}
void dxImGuiRender::OnDeviceDestroy()
{
#if defined(XR_VULKAN)
    imgui_destroy_objects();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.DrawCallback_ResetRenderState = nullptr;
    platform.DrawCallback_SetSamplerLinear = nullptr;
    platform.DrawCallback_SetSamplerNearest = nullptr;
#elif defined(USE_DX11)
    ImGui_ImplDX11_Shutdown();
#elif defined(USE_OGL)
    ImGui_ImplOpenGL3_Shutdown();
#endif
}

void dxImGuiRender::OnDeviceResetBegin()
{
#if defined(XR_VULKAN)
    imgui_destroy_objects();
#elif defined(USE_DX11)
    ImGui_ImplDX11_InvalidateDeviceObjects();
#elif defined(USE_OGL)
    ImGui_ImplOpenGL3_DestroyDeviceObjects();
#endif
}

void dxImGuiRender::OnDeviceResetEnd()
{
#if defined(XR_VULKAN)
    imgui_create_objects();
#elif defined(USE_DX11)
    ImGui_ImplDX11_CreateDeviceObjects();
#elif defined(USE_OGL)
    ImGui_ImplOpenGL3_CreateDeviceObjects();
#endif
}
} // namespace xray::render::RENDER_NAMESPACE
