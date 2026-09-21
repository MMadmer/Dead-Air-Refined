// Copyright (c) 2026 XFined-Ray
// Released under the MIT licence, see License.txt

#include "stdafx.h"

#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "Layers/xrRender/ResourceManager.h"

#include <DirectXPackedVector.h>

namespace xray::render::RENDER_NAMESPACE
{
using namespace DirectX;

// r4_rendertarget_phase_water_ripple.cpp. A replaced bake means a different level, and the sim's
// accumulator, step clock and priming flag all belong to the old one.
extern void da_water_ripple_reset();
// The visor keeps a field of its own with the same second-load hazard: it is not recreated
// between levels either, and a mask still streaming with the last level's rain is the same bug.
extern void da_visor_drops_reset();

namespace
{
// The baked water field is NOT a render target: nothing in the frame draws into it. It is a CPU
// bake (Level_load.cpp) handed to the GPU as a plain immutable 2D texture under the name every
// water shader binds, exactly the way the fluid subsystem publishes its jitter and HHGG tables.
//
// The CTexture object is created once and kept for the life of the renderer; only the D3D
// surface under it is made again per bake. That is not tidiness. Compiled shaders hold POINTERS
// to this object in their pass texture lists, so dropping it and asking the registry for it
// again across a level change is precisely the crash that was fixed in the fluid subsystem
// (see the note above dx113DFluidManager::RebindResources).
ref_texture g_field_tex;

// The puddle fill map, baked in the same sweep from the same heights and published beside the
// field under its own name. A separate texture rather than a fifth channel of the field's
// because the readers are different - the terrain G-buffer shader and the puddle reflection
// overlay bind this one and never touch the water field - and because one channel of R16F is a
// quarter of the bandwidth of the RGBA16F it would otherwise have to sample.
//
// Everything said above about g_field_tex's lifetime applies here word for word: the object is
// made once and only ever handed a new surface.
ref_texture g_fill_tex;

// True while the surfaces hold a real bake rather than the 1x1 placeholder.
bool g_field_live{};

// What that surface was built from. The bake carries no generation counter, so the identity of
// the array plus the footprint it was rasterised into is the stamp: a new level allocates a new
// array (reset_water_field shrink_to_fit's the old one) and squares its own bounding volume, so
// the pointer, the count and the four bounds cannot all match across a load by accident.
const void* g_src_data{};
size_t g_src_count{};
float g_src_bounds[4]{};

constexpr size_t field_dim = size_t(CEnvironment::water_field_dim);
constexpr size_t field_texels = field_dim * field_dim;

bool same_source(const CEnvironment& env)
{
    const Fbox& b = env.water_field_bounds;
    return g_src_data == static_cast<const void*>(env.water_field.data()) && g_src_count == env.water_field.size() &&
        g_src_bounds[0] == b.vMin.x && g_src_bounds[1] == b.vMin.z && g_src_bounds[2] == b.vMax.x &&
        g_src_bounds[3] == b.vMax.z;
}

void remember(const CEnvironment& env)
{
    const Fbox& b = env.water_field_bounds;
    g_src_data = env.water_field.data();
    g_src_count = env.water_field.size();
    g_src_bounds[0] = b.vMin.x;
    g_src_bounds[1] = b.vMin.z;
    g_src_bounds[2] = b.vMax.x;
    g_src_bounds[3] = b.vMax.z;
}

void forget()
{
    g_src_data = nullptr;
    g_src_count = 0;
    ZeroMemory(g_src_bounds, sizeof(g_src_bounds));
}

// A fresh immutable surface handed to the same CTexture. Immutable rather than a dynamic one we
// map every load: the field is written once per level and read for the whole of it, so there is
// nothing for a mapped resource to buy and one less way for a partial write to be seen.
void publish(u32 dim, const u16* texels)
{
    D3D_TEXTURE2D_DESC desc{};
    desc.Width = dim;
    desc.Height = dim;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // R = surface world Y, G = coverage, B = bed world Y, A = metres to the nearest bank.
    // The contract is da_water_field.h; changing the order here breaks every water shader.
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D_USAGE_IMMUTABLE;
    desc.BindFlags = D3D_BIND_SHADER_RESOURCE;

    D3D_SUBRESOURCE_DATA init{};
    init.pSysMem = texels;
    init.SysMemPitch = UINT(dim * 4 * sizeof(u16));

    ID3DTexture2D* surface = nullptr;
    CHK_DX(HW.pDevice->CreateTexture2D(&desc, &init, &surface));
    if (!surface)
        return;

    if (!g_field_tex)
        g_field_tex = RImplementation.Resources->_CreateTexture(r2_RT_water_field);
    g_field_tex->surface_set(surface);
    _RELEASE(surface);
}

// The fill map's surface. Single channel, half float: the stored value is 0..1 of
// CEnvironment::puddle_fill_depth, and a millimetre of standing water is far below what fp16
// loses over that range.
void publish_fill(u32 dim, const u16* texels)
{
    D3D_TEXTURE2D_DESC desc{};
    desc.Width = dim;
    desc.Height = dim;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D_USAGE_IMMUTABLE;
    desc.BindFlags = D3D_BIND_SHADER_RESOURCE;

    D3D_SUBRESOURCE_DATA init{};
    init.pSysMem = texels;
    init.SysMemPitch = UINT(dim * sizeof(u16));

    ID3DTexture2D* surface = nullptr;
    CHK_DX(HW.pDevice->CreateTexture2D(&desc, &init, &surface));
    if (!surface)
        return;

    if (!g_fill_tex)
        g_fill_tex = RImplementation.Resources->_CreateTexture(r2_RT_puddle_fill);
    g_fill_tex->surface_set(surface);
    _RELEASE(surface);
}

void publish_placeholder()
{
    const u16 zero[4]{};
    publish(1, zero);
    publish_fill(1, zero);
    g_field_live = false;
    forget();
}

// Does the named texture still own a surface. A level change unloads every texture the resource
// manager holds, and a $user$ one has no file to reload itself from - it comes back empty and
// stays that way unless somebody hands it a new surface.
bool has_surface(const ref_texture& tex)
{
    if (!tex)
        return false;
    ID3DBaseTexture* s = tex->surface_get(); // AddRefs
    if (!s)
        return false;
    _RELEASE(s);
    return true;
}
} // namespace

// Called at renderer create, before any level exists. Both names MUST resolve from the very first
// frame and on levels that have no water at all: an SRV slot that no pass fills keeps whatever
// the previous draw bound there, and the water shaders sample this one unconditionally
// (da_water_map2.w only tells them whether to believe what comes back).
void da_water_field_create_placeholder()
{
    publish_placeholder();
}

void da_water_field_release()
{
    g_field_tex = nullptr;
    g_fill_tex = nullptr;
    g_field_live = false;
    forget();
}

// Once per frame, early. Four compares and a return on every frame but the one after a bake.
void da_water_field_update()
{
    if (!g_pGamePersistent)
        return;

    const auto& env = g_pGamePersistent->Environment();
    const bool valid = env.water_field_valid() && env.water_field.size() == field_texels;

    if (has_surface(g_field_tex) && has_surface(g_fill_tex) && valid == g_field_live &&
        (!valid || same_source(env)))
        return;

    if (!valid)
    {
        publish_placeholder();
        da_water_ripple_reset();
        da_visor_drops_reset();
        return;
    }

    // 8 MB of scratch for the duration of one upload, once per level. Converted a texel at a
    // time rather than through a float staging copy: the staging array would be 16 MB more of
    // the same work.
    xr_vector<u16> packed(field_texels * 4);
    xr_vector<u16> fill(field_texels);
    // CEnvironment keeps the two arrays the same length or the fill empty; the test is what
    // makes "empty" upload as a dry map instead of walking off the end of it.
    const bool has_fill = env.puddle_fill.size() == field_texels;
    const auto* src = env.water_field.data();
    for (size_t i = 0; i < field_texels; ++i)
    {
        fill[i] = PackedVector::XMConvertFloatToHalf(has_fill ? env.puddle_fill[i] : 0.f);
        const CEnvironment::SWaterTexel& t = src[i];
        const bool wet = t.mask != 0;
        u16* d = &packed[i * 4];
        // Heights are zeroed where there is no coverage instead of carried through: the shader
        // reads them only behind the coverage test, and a defined zero cannot decode to a NaN.
        d[0] = PackedVector::XMConvertFloatToHalf(wet ? t.surface : 0.f);
        d[1] = PackedVector::XMConvertFloatToHalf(wet ? 1.f : 0.f);
        d[2] = PackedVector::XMConvertFloatToHalf(wet ? t.bed : 0.f);
        // Written on dry texels too - that is what makes the bank distance readable from both
        // sides of the shoreline.
        d[3] = PackedVector::XMConvertFloatToHalf(t.shore);
    }

    publish(u32(field_dim), packed.data());
    publish_fill(u32(field_dim), fill.data());
    g_field_live = true;
    remember(env);

    // The bake changed, so the level did. This is the only place in the renderer that notices,
    // and the ripple pair is not recreated between levels: without this the new level starts
    // with the old one's heights still ringing in it.
    da_water_ripple_reset();
    da_visor_drops_reset();
}
} // namespace xray::render::RENDER_NAMESPACE
