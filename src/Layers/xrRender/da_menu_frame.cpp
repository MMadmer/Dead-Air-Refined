// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"
#include "da_menu_frame.h"

namespace xray::render::RENDER_NAMESPACE
{
namespace
{
bool requested = false;
ref_texture frame;
ID3DTexture2D* surface = nullptr;
} // namespace

void da_menu_frame_request() { requested = true; }

void da_menu_frame_capture()
{
    if (!requested)
        return;
    requested = false;

    ID3DResource* resource = nullptr;
    RImplementation.Target->get_base_rt()->GetResource(&resource);
    if (!resource)
        return;
    ID3DTexture2D* source = static_cast<ID3DTexture2D*>(resource);
    D3D_TEXTURE2D_DESC desc;
    source->GetDesc(&desc);

    // A surface of the frame's own size and format, made again when the screen changed.
    D3D_TEXTURE2D_DESC have{};
    if (surface)
        surface->GetDesc(&have);
    if (!surface || have.Width != desc.Width || have.Height != desc.Height || have.Format != desc.Format)
    {
        D3D_TEXTURE2D_DESC copy = desc;
        copy.MipLevels = 1;
        copy.ArraySize = 1;
        copy.SampleDesc.Count = 1;
        copy.SampleDesc.Quality = 0;
        copy.Usage = D3D_USAGE_DEFAULT;
        copy.BindFlags = D3D_BIND_SHADER_RESOURCE;
        copy.CPUAccessFlags = 0;
        copy.MiscFlags = 0;
        ID3DTexture2D* made = nullptr;
        if (desc.SampleDesc.Count > 1 || FAILED(HW.pDevice->CreateTexture2D(&copy, nullptr, &made)))
        {
            Msg("! [menu] no copy of the game's frame (format %u)", u32(desc.Format));
            _RELEASE(resource);
            return;
        }
        _RELEASE(surface);
        surface = made;
        if (!frame)
            frame.create("$user$game_frame");
        frame->surface_set(surface);
    }
    HW.get_context(CHW::IMM_CTX_ID)->CopyResource(surface, source);
    _RELEASE(resource);
}

void da_menu_frame_destroy()
{
    requested = false;
    frame.destroy();
    _RELEASE(surface);
}
} // namespace xray::render::RENDER_NAMESPACE
