#include "stdafx.h"
#pragma hdrstop

#include <DirectXTex.h>

namespace xray::render::RENDER_NAMESPACE
{
void fix_texture_name(pstr fn)
{
    pstr _ext = strext(fn);
    if (_ext && (!xr_stricmp(_ext, ".tga") || !xr_stricmp(_ext, ".dds") || !xr_stricmp(_ext, ".bmp") ||
        !xr_stricmp(_ext, ".ogm")))
    {
        *_ext = 0;
    }
}

int get_texture_load_lod(LPCSTR fn)
{
    CInifile::Sect& sect = pSettings->r_section("reduce_lod_texture_list");
    auto it_ = sect.Data.cbegin();
    auto it_e_ = sect.Data.cend();

    ENGINE_API bool is_enough_address_space_available();
    static bool enough_address_space_available = is_enough_address_space_available();

    auto it = it_;
    auto it_e = it_e_;

    for (; it != it_e; ++it)
    {
        if (strstr(fn, it->first.c_str()))
        {
            if (psTextureLOD < 1)
            {
                if (enough_address_space_available)
                    return 0;
                else
                    return 1;
            }
            else if (psTextureLOD < 3)
                return 1;
            else
                return 2;
        }
    }

    if (psTextureLOD < 2)
    {
        //if (enough_address_space_available)
        return 0;
        //else
        //    return 1;
    }
    else if (psTextureLOD < 4)
        return 1;
    else
        return 2;
}

u32 calc_texture_size(int lod, u32 mip_cnt, size_t orig_size)
{
    if (1 == mip_cnt)
        return orig_size;

    int _lod = lod;
    float res = float(orig_size);

    while (_lod > 0)
    {
        --_lod;
        res -= res / 1.333f;
    }
    return iFloor(res);
}

//////////////////////////////////////////////////////////////////////
// Utility pack
//////////////////////////////////////////////////////////////////////

IC void Reduce(size_t& w, size_t& h, size_t& l, int skip)
{
    while ((l > 1) && skip)
    {
        w /= 2;
        h /= 2;
        l -= 1;

        skip--;
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
}

namespace
{
constexpr u32 ddsMagic = 0x20534444;
constexpr u32 ddsHeaderSize = 124;
constexpr u32 ddsPixelFormatSize = 32;
constexpr u32 ddsFourCcDx10 = 0x30315844;

HRESULT create_texture_from_block_compressed_dds(
    const u8* data, size_t size, int requestedLod, ID3DBaseTexture** result, size_t& loadedMipCount)
{
    if (!data || !result || size < 128)
        return E_INVALIDARG;

    u32 magic = 0;
    u32 headerSize = 0;
    u32 pixelFormatSize = 0;
    u32 fourCc = 0;
    memcpy(&magic, data, sizeof(magic));
    memcpy(&headerSize, data + 4, sizeof(headerSize));
    memcpy(&pixelFormatSize, data + 76, sizeof(pixelFormatSize));
    memcpy(&fourCc, data + 84, sizeof(fourCc));
    if (magic != ddsMagic || headerSize != ddsHeaderSize || pixelFormatSize != ddsPixelFormatSize)
        return E_FAIL;

    DirectX::TexMetadata metadata;
    const auto metadataResult =
        DirectX::GetMetadataFromDDSMemory(data, size, DirectX::DDS_FLAGS_PERMISSIVE, metadata);
    if (FAILED(metadataResult) || !DirectX::IsCompressed(metadata.format))
        return E_FAIL;

    size_t dataOffset = 128;
    if (fourCc == ddsFourCcDx10)
        dataOffset += 20;
    if (dataOffset >= size || !metadata.mipLevels || !metadata.arraySize)
        return E_FAIL;

    const size_t skippedMips = metadata.IsCubemap() ?
        0 :
        std::min<size_t>(std::max(requestedLod, 0), metadata.mipLevels - 1);
    const size_t outputMipCount = metadata.mipLevels - skippedMips;
    const size_t itemCount = metadata.dimension == DirectX::TEX_DIMENSION_TEXTURE3D ? 1 : metadata.arraySize;

    xr_vector<D3D11_SUBRESOURCE_DATA> subresources;
    subresources.reserve(outputMipCount * itemCount);
    const u8* source = data + dataOffset;
    const u8* const sourceEnd = data + size;

    for (size_t item = 0; item < itemCount; ++item)
    {
        size_t width = metadata.width;
        size_t height = metadata.height;
        size_t depth = metadata.depth;
        for (size_t mip = 0; mip < metadata.mipLevels; ++mip)
        {
            size_t rowPitch = 0;
            size_t slicePitch = 0;
            const auto pitchResult =
                DirectX::ComputePitch(metadata.format, width, height, rowPitch, slicePitch);
            if (FAILED(pitchResult) || slicePitch > std::numeric_limits<UINT>::max())
                return E_FAIL;

            const size_t subresourceSize = slicePitch * depth;
            if (subresourceSize > static_cast<size_t>(sourceEnd - source))
                return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);

            if (mip >= skippedMips)
            {
                D3D11_SUBRESOURCE_DATA& subresource = subresources.emplace_back();
                subresource.pSysMem = source;
                subresource.SysMemPitch = static_cast<UINT>(rowPitch);
                subresource.SysMemSlicePitch = static_cast<UINT>(slicePitch);
            }

            source += subresourceSize;
            width = std::max<size_t>(1, width >> 1);
            height = std::max<size_t>(1, height >> 1);
            depth = std::max<size_t>(1, depth >> 1);
        }
    }

    const UINT outputWidth = static_cast<UINT>(std::max<size_t>(1, metadata.width >> skippedMips));
    const UINT outputHeight = static_cast<UINT>(std::max<size_t>(1, metadata.height >> skippedMips));
    const UINT outputDepth = static_cast<UINT>(std::max<size_t>(1, metadata.depth >> skippedMips));
    HRESULT createResult = E_FAIL;

    switch (metadata.dimension)
    {
    case DirectX::TEX_DIMENSION_TEXTURE1D:
    {
        D3D11_TEXTURE1D_DESC description{};
        description.Width = outputWidth;
        description.MipLevels = static_cast<UINT>(outputMipCount);
        description.ArraySize = static_cast<UINT>(metadata.arraySize);
        description.Format = metadata.format;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture1D* texture = nullptr;
        createResult = HW.pDevice->CreateTexture1D(&description, subresources.data(), &texture);
        *result = texture;
        break;
    }
    case DirectX::TEX_DIMENSION_TEXTURE2D:
    {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = outputWidth;
        description.Height = outputHeight;
        description.MipLevels = static_cast<UINT>(outputMipCount);
        description.ArraySize = static_cast<UINT>(metadata.arraySize);
        description.Format = metadata.format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        description.MiscFlags = metadata.IsCubemap() ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        ID3D11Texture2D* texture = nullptr;
        createResult = HW.pDevice->CreateTexture2D(&description, subresources.data(), &texture);
        *result = texture;
        break;
    }
    case DirectX::TEX_DIMENSION_TEXTURE3D:
    {
        D3D11_TEXTURE3D_DESC description{};
        description.Width = outputWidth;
        description.Height = outputHeight;
        description.Depth = outputDepth;
        description.MipLevels = static_cast<UINT>(outputMipCount);
        description.Format = metadata.format;
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture3D* texture = nullptr;
        createResult = HW.pDevice->CreateTexture3D(&description, subresources.data(), &texture);
        *result = texture;
        break;
    }
    default:
        return E_FAIL;
    }

    if (SUCCEEDED(createResult))
        loadedMipCount = outputMipCount;
    return createResult;
}
}

ID3DBaseTexture* CRender::texture_load(LPCSTR fRName, u32& ret_msize)
{
    ret_msize = 0;
    R_ASSERT1_CURE(fRName && fRName[0], { return nullptr; });

    ID3DBaseTexture* pTexture2D{};
    string_path fn;
    {
        // make file name
        string_path fname;
        xr_strcpy(fname, fRName);
        fix_texture_name(fname);

        const bool isBump = strstr(fname, "_bump");
        if (isBump)
        {
            string_path gameTexturePath;
            if (!FS.exist(gameTexturePath, "$game_textures$", fname, ".dds"))
            {
                Msg("* Fallback to default bump map: %s", fname);
                if (strstr(fname, "_bump#"))
                    R_ASSERT1_CURE(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_bump#", ".dds"), return nullptr);
                else
                    R_ASSERT1_CURE(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_bump", ".dds"), return nullptr);
            }
            else
            {
                if (!FS.exist(fn, "$level$", fname, ".dds") &&
                    !FS.exist(fn, "$game_saves$", fname, ".dds"))
                {
                    xr_strcpy(fn, gameTexturePath);
                }
            }
        }
        else
        {
            bool exist = false;

            for (cpcstr folder : { "$level$", "$game_saves$", "$game_textures$" })
            {
                exist = FS.exist(fn, folder, fname, ".dds");
                if (exist)
                    break;
            }

            if (!exist)
            {
                Msg("! Can't find texture '%s'", fname);
                if (!FS.exist(fn, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"))
                    return nullptr;
            }
        }
    }

    // Load and get header
    IReader* S = FS.r_open(fn);
    R_ASSERT3_CURE(S, "Can't open texture", fn, { return nullptr; });

    size_t img_size = S->length();
#ifdef DEBUG
    Msg("* Loaded: %s[%zu]", fn, img_size);
#endif // DEBUG

    DirectX::DDS_FLAGS dds_flags{ DirectX::DDS_FLAGS_PERMISSIVE };
    xr_strlwr(fn);
    const int img_loaded_lod = get_texture_load_lod(fn);
    size_t fastMipCount = 0;
    auto fastResult = create_texture_from_block_compressed_dds(
        static_cast<const u8*>(S->pointer()), img_size, img_loaded_lod, &pTexture2D, fastMipCount);
    if (SUCCEEDED(fastResult))
    {
        ret_msize = calc_texture_size(img_loaded_lod, static_cast<u32>(fastMipCount), img_size);
        FS.r_close(S);
        return pTexture2D;
    }

    for (int i = 1; i <= 3; ++i) // 3 attempts
    {
        DirectX::TexMetadata IMG;
        DirectX::ScratchImage texture;
        const u8* ddsData = static_cast<u8*>(S->pointer());
        xr_vector<u8> normalizedDDS;

        // Legacy D3DX accepted DDS_HEADER.dwSize including the four-byte magic.
        if (img_size >= 8 && !memcmp(ddsData, "DDS ", 4))
        {
            u32 headerSize;
            memcpy(&headerSize, ddsData + 4, sizeof(headerSize));
            if (headerSize == 128)
            {
                normalizedDDS.assign(ddsData, ddsData + img_size);
                headerSize = 124;
                memcpy(normalizedDDS.data() + 4, &headerSize, sizeof(headerSize));
                ddsData = normalizedDDS.data();
            }
        }

        auto hresult = LoadFromDDSMemory(ddsData, img_size, dds_flags, &IMG, texture);

        if (FAILED(hresult))
        {
            Msg("! DDS load failed: HRESULT=0x%08X, size=%zu, file=%s", static_cast<u32>(hresult), img_size, fn);
            if (IWriter* dump = FS.w_open("$logs$", "failed_texture.dds"))
            {
                dump->w(S->begin(), S->length());
                FS.w_close(dump);
            }
        }

        R_ASSERT3_CURE(SUCCEEDED(hresult), "Failed to load texture from memory", fn,
        {
            FS.r_close(S);
            return nullptr;
        });

        size_t mip_lod = 0;
        if (img_loaded_lod && !IMG.IsCubemap())
        {
            const auto old_mipmap_cnt = IMG.mipLevels;
            Reduce(IMG.width, IMG.height, IMG.mipLevels, img_loaded_lod);
            mip_lod = old_mipmap_cnt - IMG.mipLevels;
        }

        // DirectX requires compressed texture size to be
        // a multiple of 4. Make sure to meet this requirement.
        if (DirectX::IsCompressed(IMG.format))
        {
            IMG.width = (IMG.width + 3u) & ~0x3u;
            IMG.height = (IMG.height + 3u) & ~0x3u;
        }

        hresult = CreateTextureEx(HW.pDevice, texture.GetImages() + mip_lod, texture.GetImageCount(), IMG,
            D3D_USAGE_IMMUTABLE, D3D_BIND_SHADER_RESOURCE, 0, IMG.miscFlags, DirectX::CREATETEX_DEFAULT,
            &pTexture2D);

        if (SUCCEEDED(hresult))
        {
            // OK
            ret_msize = calc_texture_size(img_loaded_lod, IMG.mipLevels, img_size);
            break;
        }

        if (i == 1)
            dds_flags |= DirectX::DDS_FLAGS::DDS_FLAGS_NO_16BPP; // System isn't WDDM 1.2 compliant
        else if (i == 2)
            dds_flags |= DirectX::DDS_FLAGS::DDS_FLAGS_FORCE_RGB; // Not even WDDM 1.1 compliant
        else if (i == 3)
            Msg("! Could not load texture [%s] after %d attempts", fn, i);
        else
        {
            NODEFAULT;
        }
    }

    FS.r_close(S);
    return pTexture2D;
}
} // namespace xray::render::RENDER_NAMESPACE
