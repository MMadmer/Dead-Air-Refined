#include "stdafx.h"

#include "TextureDescrManager.h"
#include "ETextureParams.h"

#include "xrCore/Threading/ParallelForEach.hpp"
#include "xrCore/XMS/xms_assets.h"

namespace xray::render::RENDER_NAMESPACE
{
// eye-params
float r__dtex_range = 50;
class cl_dt_scaler : public R_constant_setup
{
public:
    float scale;

    cl_dt_scaler(float s) : scale(s) {}
    void setup(CBackend& cmd_list, R_constant* C) override { cmd_list.set_c(C, scale, scale, scale, 1 / r__dtex_range); }
};

void fix_texture_thm_name(pstr fn)
{
    pstr _ext = strext(fn);
    if (_ext && (!xr_stricmp(_ext, ".tga") || !xr_stricmp(_ext, ".thm") || !xr_stricmp(_ext, ".dds") ||
       !xr_stricmp(_ext, ".bmp") || !xr_stricmp(_ext, ".ogm")))
    {
        *_ext = 0;
    }
}

void CTextureDescrMngr::LoadLTX(pcstr initial, bool listTHM)
{
    ZoneScoped;

    string_path fname;
    FS.update_path(fname, initial, "textures.ltx");

    if (!FS.exist(fname))
        return;

#ifndef MASTER_GOLD
    Msg("Processing textures.ltx in [%s]", initial);
#endif

    CInifile ini(fname);

    Lock lock;
    if (ini.section_exist("association"))
    {
        CInifile::Sect& data = ini.r_section("association");
#ifndef MASTER_GOLD
        Msg("\tsection [%s] has %d lines", data.Name.c_str(), data.Data.size());
#endif
        m_texture_details.reserve(m_texture_details.size() + data.Data.size());
        m_detail_scalers.reserve(m_detail_scalers.size() + data.Data.size());

        const auto processAssociation = [&](const CInifile::Item& item)
        {
            ZoneScopedN("Process association");
            if (listTHM)
                Msg("\t\t%s = %s", item.first.c_str(), item.second.c_str());

            lock.Enter();
            texture_desc& desc = m_texture_details[item.first];
            cl_dt_scaler*& dts = m_detail_scalers[item.first];
            lock.Leave();

            if (desc.m_assoc)
                xr_delete(desc.m_assoc);

            desc.m_assoc = xr_new<texture_assoc>();

            string_path T;
            float s;

            const int res = sscanf(item.second.c_str(), "%[^,],%f", T, &s);
            R_ASSERT4(res == 2, "Bad texture association", item.first.c_str(), fname);
            desc.m_assoc->detail_name = T;
            if (dts)
                dts->scale = s;
            else
                dts = xr_new<cl_dt_scaler>(s);

            if (strstr(item.second.c_str(), "usage[diffuse_or_bump]"))
                desc.m_assoc->usage.set(texture_assoc::flDiffuseDetail | texture_assoc::flBumpDetail);
            else if (strstr(item.second.c_str(), "usage[bump]"))
                desc.m_assoc->usage.set(texture_assoc::flBumpDetail);
            else if (strstr(item.second.c_str(), "usage[diffuse]"))
                desc.m_assoc->usage.set(texture_assoc::flDiffuseDetail);
        };
        xr_parallel_for_each(data.Data, processAssociation);
    } // "association"

    if (ini.section_exist("specification"))
    {
        CInifile::Sect& data = ini.r_section("specification");
#ifndef MASTER_GOLD
        Msg("\tsection [%s] has %d lines", data.Name.c_str(), data.Data.size());
#endif
        m_texture_details.reserve(m_texture_details.size() + data.Data.size());

        const auto processSpecification = [&](const CInifile::Item& item)
        {
            if (listTHM)
                Msg("\t\t%s = %s", item.first.c_str(), item.second.c_str());

            lock.Enter();
            texture_desc& desc = m_texture_details[item.first];
            lock.Leave();

            if (desc.m_spec)
                xr_delete(desc.m_spec);

            desc.m_spec = xr_new<texture_spec>();

            string_path bmode;
            const int res =
                    sscanf(item.second.c_str(), "bump_mode[%[^]]], material[%f]", bmode, &desc.m_spec->m_material);
            R_ASSERT4(res == 2, "Bad texture specification", item.first.c_str(), fname);
            if ((bmode[0] == 'u') && (bmode[1] == 's') && (bmode[2] == 'e') && (bmode[3] == ':'))
            {
                // bump-map specified
                desc.m_spec->m_bump_name = bmode + 4;
            }
        };
        xr_parallel_for_each(data.Data, processSpecification);
    } // "specification"
}

void CTextureDescrMngr::LoadTHM(LPCSTR initial, bool listTHM)
{
    ZoneScoped;

    FS_FileSet flist;
    FS.file_list(flist, initial, FS_ListFiles, "*.thm");

    if (flist.empty())
        return;

#ifndef MASTER_GOLD
    Msg("Processing %d .thm files in [%s]", flist.size(), initial);
#endif


    m_texture_details.reserve(m_texture_details.size() + flist.size());
    m_detail_scalers.reserve(m_detail_scalers.size() + flist.size());

    Lock lock;
    const auto processFile = [&](const FS_File& it)
    {
        ZoneScopedN("Process file");
        // Alundaio: Print list of *.thm to find bad .thms!
        if (listTHM)
            Log("\t", it.name.c_str());

        string_path fn;
        FS.update_path(fn, initial, it.name.c_str());
        IReader* F = FS.r_open(fn);
        R_ASSERT3(F, "Failed to open THM (case-sensitivity problem?)", it.name.c_str());

        xr_strcpy(fn, it.name.c_str());
        fix_texture_thm_name(fn);

        R_ASSERT3(F->find_chunk(THM_CHUNK_TYPE), "Cannot find THM chunk in file", fn);
        F->r_u32();
        STextureParams tp;
        tp.Load(*F);
        FS.r_close(F);
        if (STextureParams::ttImage == tp.type || STextureParams::ttTerrain == tp.type ||
            STextureParams::ttNormalMap == tp.type)
        {
            lock.Enter();
            texture_desc& desc = m_texture_details[fn];
            cl_dt_scaler*& dts = m_detail_scalers[fn];
            lock.Leave();

            if (tp.detail_name.size() &&
                tp.flags.is_any(STextureParams::flDiffuseDetail | STextureParams::flBumpDetail))
            {
                if (desc.m_assoc)
                    xr_delete(desc.m_assoc);

                desc.m_assoc = xr_new<texture_assoc>();
                desc.m_assoc->detail_name = tp.detail_name;
                if (dts)
                    dts->scale = tp.detail_scale;
                else
                    dts = xr_new<cl_dt_scaler>(tp.detail_scale);

                if (tp.flags.is(STextureParams::flDiffuseDetail))
                    desc.m_assoc->usage.set(texture_assoc::flDiffuseDetail);

                if (tp.flags.is(STextureParams::flBumpDetail))
                    desc.m_assoc->usage.set(texture_assoc::flBumpDetail);
            }
            if (desc.m_spec)
                xr_delete(desc.m_spec);

            desc.m_spec = xr_new<texture_spec>();
            desc.m_spec->m_material = float(tp.material) + tp.material_weight;
            desc.m_spec->m_use_steep_parallax = false;

            if (tp.bump_mode == STextureParams::tbmUse)
            {
                desc.m_spec->m_bump_name = tp.bump_name;
            }
            else if (tp.bump_mode == STextureParams::tbmUseParallax)
            {
                desc.m_spec->m_bump_name = tp.bump_name;
                desc.m_spec->m_use_steep_parallax = true;
            }
        }
    };
    if (!listTHM)
        xr_parallel_for_each(flist, processFile);
    else
    {
        // We need precise info in the log when listTHM is specified.
        // Load in single thread, file by file.
        for (const FS_File& file : flist)
            processFile(file);
    }
}

void CTextureDescrMngr::ApplyDescriptions()
{
    for (const auto& [name, tex] : XMS::Assets::Textures())
    {
        if (!tex.described)
            continue;
        const shared_str key = name.c_str();
        texture_desc& desc = m_texture_details[key];
        const auto spec = [&desc]() -> texture_spec&
        {
            if (!desc.m_spec)
            {
                desc.m_spec = xr_new<texture_spec>();
                desc.m_spec->m_material = 1.0f; // what GetMaterial answers for a texture nobody describes
                desc.m_spec->m_use_steep_parallax = false;
            }
            return *desc.m_spec;
        };

        if (tex.bump == 0 && desc.m_spec)
        {
            desc.m_spec->m_bump_name = shared_str();
            desc.m_spec->m_use_steep_parallax = false;
        }
        else if (tex.bump == 1)
        {
            spec().m_bump_name = tex.bump_named ? tex.bump_name.c_str() : (name + "_bump").c_str();
            spec().m_use_steep_parallax = tex.parallax;
        }
        if (tex.has_material)
            spec().m_material = tex.material;

        if (tex.detail == 0)
            xr_delete(desc.m_assoc);
        else if (tex.detail == 1)
        {
            xr_delete(desc.m_assoc);
            desc.m_assoc = xr_new<texture_assoc>();
            desc.m_assoc->detail_name = tex.detail_name.c_str();
            if (tex.detail_diffuse)
                desc.m_assoc->usage.set(texture_assoc::flDiffuseDetail);
            if (tex.detail_bump)
                desc.m_assoc->usage.set(texture_assoc::flBumpDetail);
            cl_dt_scaler*& dts = m_detail_scalers[key];
            if (dts)
                dts->scale = tex.detail_scale;
            else
                dts = xr_new<cl_dt_scaler>(tex.detail_scale);
        }
    }
}

void CTextureDescrMngr::Load()
{
    ZoneScoped;
#ifndef MASTER_GOLD
    CTimer timer;
    timer.Start();
#endif // #ifdef DEBUG

    const bool listTHM = strstr(Core.Params, "-list_thm");

    LoadLTX("$game_textures$", listTHM);
    LoadLTX("$level$", listTHM);

    LoadTHM("$game_textures$", listTHM);
    LoadTHM("$level$", listTHM);

    ApplyDescriptions();

#ifndef MASTER_GOLD
    Msg("%s, texture descriptions loaded for %d ms", __FUNCTION__, timer.GetElapsed_ms());
#endif
}

void CTextureDescrMngr::UnLoad()
{
    for (auto& it : m_texture_details)
    {
        xr_delete(it.second.m_assoc);
        xr_delete(it.second.m_spec);
    }
    m_texture_details.clear();
}

CTextureDescrMngr::~CTextureDescrMngr()
{
    ZoneScoped;

    for (auto& it : m_detail_scalers)
        xr_delete(it.second);

    m_detail_scalers.clear();
}

namespace
{
// A level piece's own texture goes by "<texture>@piece<n>": it is the texture of that name with the piece's light in
// it, and it is described - its bump, its detail texture, its material - as that texture is.
shared_str described_name(const shared_str& tex_name)
{
    pcstr name = tex_name.c_str();
    pcstr at = name ? strstr(name, "@piece") : nullptr;
    if (!at)
        return tex_name;
    string_path base;
    strncpy_s(base, name, size_t(at - name));
    return base;
}
} // namespace

shared_str CTextureDescrMngr::GetBumpName(const shared_str& tex_name) const
{
    map_TD::const_iterator I = m_texture_details.find(described_name(tex_name));
    if (I != m_texture_details.end())
    {
        if (I->second.m_spec)
        {
            return I->second.m_spec->m_bump_name;
        }
    }
    return "";
}

BOOL CTextureDescrMngr::UseSteepParallax(const shared_str& tex_name) const
{
    map_TD::const_iterator I = m_texture_details.find(described_name(tex_name));
    if (I != m_texture_details.end())
    {
        if (I->second.m_spec)
        {
            if (I->second.m_spec->m_use_steep_parallax)
                return TRUE;

            // r__parallax_force: the sibling's census over 5985 .thm files found 112
            // textures hand-marked for parallax against 2020 carrying a height map - the
            // GSC editor flag simply never reached most of them. A non-empty bump name
            // means a _bump/_bump# pair exists with height in the alpha of the second -
            // the exact data UpdateTC reads, same signal, different author.
            // Needs a level reload: shader names are built once at blender compile.
            if (ps_r__parallax_force && I->second.m_spec->m_bump_name.size() > 2)
                return TRUE;
        }
    }
    return FALSE;
}

float CTextureDescrMngr::GetMaterial(const shared_str& tex_name) const
{
    map_TD::const_iterator I = m_texture_details.find(described_name(tex_name));
    if (I != m_texture_details.end())
    {
        if (I->second.m_spec)
        {
            return I->second.m_spec->m_material;
        }
    }
    return 1.0f;
}

void CTextureDescrMngr::GetTextureUsage(const shared_str& tex_name, bool& bDiffuse, bool& bBump) const
{
    map_TD::const_iterator I = m_texture_details.find(described_name(tex_name));
    if (I != m_texture_details.end())
    {
        if (I->second.m_assoc)
        {
            auto& usage = I->second.m_assoc->usage;
            bDiffuse = usage.test(texture_assoc::flDiffuseDetail);
            bBump = usage.test(texture_assoc::flBumpDetail);
        }
    }
}

BOOL CTextureDescrMngr::GetDetailTexture(const shared_str& tex_name, LPCSTR& res, R_constant_setup*& CS) const
{
    map_TD::const_iterator I = m_texture_details.find(described_name(tex_name));
    if (I != m_texture_details.end())
    {
        if (I->second.m_assoc)
        {
            texture_assoc* TA = I->second.m_assoc;
            res = TA->detail_name.c_str();
            map_CS::const_iterator It2 = m_detail_scalers.find(described_name(tex_name));
            CS = It2 == m_detail_scalers.end() ? 0 : It2->second;
            return TRUE;
        }
    }
    return FALSE;
}
} // namespace xray::render::RENDER_NAMESPACE
