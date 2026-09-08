#include "stdafx.h"

#include "PSLibrary.h"
#include "ParticleEffect.h"
#include "ParticleGroup.h"
#ifdef _EDITOR
#include "ParticleEffectActions.h"
#include "editors/ECore/Editor/ui_main.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
bool ped_sort_pred(const PS::CPEDef* a, const PS::CPEDef* b) { return xr_strcmp(a->Name(), b->Name()) < 0; }
bool pgd_sort_pred(const PS::CPGDef* a, const PS::CPGDef* b) { return xr_strcmp(a->m_Name, b->m_Name) < 0; }
//----------------------------------------------------
void CPSLibrary::OnCreate()
{
    ZoneScoped;
#ifdef _EDITOR
    if (pCreateEAction)
    {
        Load2();
    }
    else
#endif
    {
        string_path fn;
        FS.update_path(fn, _game_data_, "particles.xr");
        Load(fn);
        LoadLooseOverrides();
        ResolveWindScales();
        ResolveShaderFire();
    }
}

// Which effects drift in the wind and how much. The rule table is data (the mechanism lives
// here, the names in dead_air_x64_wind.ltx): an exact effect name wins, otherwise the LONGEST
// substring key found in the name decides, and an effect no key matches takes no wind at all.
// So "anomaly" (0) beats "smoke" (1) inside an anomaly's smoke, while a campfire's smoke drifts
// and its flame stays over the logs.
void CPSLibrary::ResolveWindScales()
{
    ZoneScoped;

    xr_vector<std::pair<xr_string, float>> table;
    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_wind.ltx");
    if (FS.exist(path))
    {
        CInifile ini(path, TRUE);
        if (ini.section_exist("particle_wind"))
            for (const auto& item : ini.r_section("particle_wind").Data)
                if (item.first.size() && item.second.size())
                {
                    xr_string key(item.first.c_str());
                    std::transform(key.begin(), key.end(), key.begin(), [](char c) { return char(tolower(c)); });
                    table.emplace_back(std::move(key), clampr(float(atof(item.second.c_str())), 0.f, 1.f));
                }
    }
    // Longest key first: the first substring hit is the longest one.
    std::sort(table.begin(), table.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

    u32 windy = 0;
    for (PS::CPEDef* def : m_PEDs)
    {
        xr_string name(def->Name() ? def->Name() : "");
        std::transform(name.begin(), name.end(), name.begin(), [](char c) { return char(tolower(c)); });
        float scale = 0.f;
        bool exact = false;
        for (const auto& [key, value] : table)
            if (key == name)
            {
                scale = value;
                exact = true;
                break;
            }
        if (!exact)
            for (const auto& [key, value] : table)
                if (name.find(key) != xr_string::npos)
                {
                    scale = value;
                    break;
                }
        def->m_WindScale = scale;
        if (scale > 0.f)
            ++windy;
    }
    Msg("* [wind] particle wind: %u rule(s), %u of %u effect(s) drift", u32(table.size()), windy, u32(m_PEDs.size()));
}

// Which effects are drawn as a shader fire (CDaFireEffect) instead of their sprites: the
// [shader_fire] table of dead_air_x64_fire.ltx names an effect and its preset ("off" for an
// effect that should vanish, such as the glow sprite the shader flame replaces).
void CPSLibrary::ResolveShaderFire()
{
    string_path path;
    FS.update_path(path, "$game_config$", "dead_air_x64_fire.ltx");
    if (!FS.exist(path))
        return;
    CInifile ini(path, TRUE);
    if (!ini.section_exist("shader_fire"))
        return;
    u32 mapped = 0;
    for (const auto& item : ini.r_section("shader_fire").Data)
    {
        if (!item.first.size() || !item.second.size())
            continue;
        PS::CPEDef* def = FindPED(item.first.c_str());
        if (!def)
        {
            Msg("! [fire] shader fire names an effect that is not in particles.xr: [%s]", item.first.c_str());
            continue;
        }
        def->m_DaFire = item.second;
        ++mapped;
    }
    if (mapped)
        Msg("* [fire] shader fire: %u effect(s) drawn as a flame volume", mapped);
}

// Per-particle overrides without rebuilding particles.xr. The whole game ships as one
// archive, so editing any single effect used to require rebuilding it with tools modders
// do not have. The single-file parsers (CPEDef::Load2 / CPGDef::Load2, ini-style .pe/.pg)
// already exist for the editor build; here they run in game too: after the archive is
// parsed, gamedata/particles is enumerated through the engine VFS and every file replaces
// the same-named archive definition or is added as a new one.
//
// The effect name is the file path without extension relative to the particles directory,
// so the on-disk layout must mirror the archive names. FindPEDIt/FindPGDIt use binary
// search in game builds: replacements keep the order, additions append, therefore one
// re-sort runs after the loop. With no loose files present this is a single VFS listing.
void CPSLibrary::LoadLooseOverrides()
{
    ZoneScoped;

    string_path path;
    FS.update_path(path, _game_data_, "particles" DELIMITER);

    FS_FileSet files;
    if (0 == FS.file_list(files, path, FS_ListFiles, "*.pe,*.pg"))
        return;

    u32 replaced_ped = 0, added_ped = 0, replaced_pgd = 0, added_pgd = 0, failed = 0;
    bool appended = false;

    string_path fn, p_dir, p_name, p_ext, name;
    for (const auto& f : files)
    {
        xr_sprintf(fn, sizeof(fn), "%s%s", path, f.name.c_str());
        _splitpath(f.name.c_str(), nullptr, p_dir, p_name, p_ext);
        xr_sprintf(name, sizeof(name), "%s%s", p_dir, p_name);

        CInifile ini(fn, TRUE, TRUE, FALSE);

        if (0 == xr_stricmp(p_ext, ".pe"))
        {
            PS::CPEDef* def = xr_new<PS::CPEDef>();
            def->m_Name = name;
            if (!def->Load2(ini))
            {
                Msg("! Loose particle effect skipped, failed to parse '%s'", fn);
                xr_delete(def);
                ++failed;
                continue;
            }

            const PS::PEDIt it = FindPEDIt(name);
            if (it != m_PEDs.end())
            {
                (*it)->DestroyShader();
                xr_delete(*it);
                *it = def;
                ++replaced_ped;
            }
            else
            {
                m_PEDs.push_back(def);
                appended = true;
                ++added_ped;
            }
            def->CreateShader();
        }
        else if (0 == xr_stricmp(p_ext, ".pg"))
        {
            PS::CPGDef* def = xr_new<PS::CPGDef>();
            def->m_Name = name;
            if (!def->Load2(ini))
            {
                Msg("! Loose particle group skipped, failed to parse '%s'", fn);
                xr_delete(def);
                ++failed;
                continue;
            }

            const PS::PGDIt it = FindPGDIt(name);
            if (it != m_PGDs.end())
            {
                xr_delete(*it);
                *it = def;
                ++replaced_pgd;
            }
            else
            {
                m_PGDs.push_back(def);
                appended = true;
                ++added_pgd;
            }
        }
    }

    if (appended)
    {
        std::sort(m_PEDs.begin(), m_PEDs.end(), ped_sort_pred);
        std::sort(m_PGDs.begin(), m_PGDs.end(), pgd_sort_pred);
    }

    Msg("* Loose particles: %u effects replaced, %u added; %u groups replaced, %u added; %u failed",
        replaced_ped, added_ped, replaced_pgd, added_pgd, failed);
}

void CPSLibrary::OnDestroy()
{
    for (PS::PEDIt e_it = m_PEDs.begin(); e_it != m_PEDs.end(); ++e_it)
        (*e_it)->DestroyShader();

    for (PS::PEDIt e_it = m_PEDs.begin(); e_it != m_PEDs.end(); ++e_it)
        xr_delete(*e_it);
    m_PEDs.clear();

    for (PS::PGDIt g_it = m_PGDs.begin(); g_it != m_PGDs.end(); ++g_it)
        xr_delete(*g_it);
    m_PGDs.clear();
}
//----------------------------------------------------
PS::PEDIt CPSLibrary::FindPEDIt(LPCSTR Name)
{
    if (!Name)
        return m_PEDs.end();
#ifdef _EDITOR
    for (PS::PEDIt it = m_PEDs.begin(); it != m_PEDs.end(); it++)
        if (0 == xr_strcmp((*it)->Name(), Name))
            return it;
    return m_PEDs.end();
#else
    PS::PEDIt I = std::lower_bound(m_PEDs.begin(), m_PEDs.end(), Name, [](const PS::CPEDef* a, pcstr b)
    {
        return xr_strcmp(a->Name(), b) < 0;
    });
    if (I == m_PEDs.end() || (0 != xr_strcmp((*I)->m_Name, Name)))
        return m_PEDs.end();
    return I;
#endif
}

PS::CPEDef* CPSLibrary::FindPED(LPCSTR Name)
{
    PS::PEDIt it = FindPEDIt(Name);
    return (it == m_PEDs.end()) ? 0 : *it;
}

PS::PGDIt CPSLibrary::FindPGDIt(LPCSTR Name)
{
    if (!Name)
        return m_PGDs.end();
#ifdef _EDITOR
    for (PS::PGDIt it = m_PGDs.begin(); it != m_PGDs.end(); it++)
        if (0 == xr_strcmp((*it)->m_Name, Name))
            return it;
    return m_PGDs.end();
#else
    PS::PGDIt I = std::lower_bound(m_PGDs.begin(), m_PGDs.end(), Name, [](const PS::CPGDef* a, pcstr b)
    {
        return xr_strcmp(a->m_Name, b) < 0;
    });
    if (I == m_PGDs.end() || (0 != xr_strcmp((*I)->m_Name, Name)))
        return m_PGDs.end();
    return I;
#endif
}

PS::CPGDef* CPSLibrary::FindPGD(LPCSTR Name)
{
    PS::PGDIt it = FindPGDIt(Name);
    return (it == m_PGDs.end()) ? 0 : *it;
}

void CPSLibrary::RenamePED(PS::CPEDef* src, LPCSTR new_name)
{
    R_ASSERT(src && new_name && new_name[0]);
    src->SetName(new_name);
}

void CPSLibrary::RenamePGD(PS::CPGDef* src, LPCSTR new_name)
{
    R_ASSERT(src && new_name && new_name[0]);
    src->SetName(new_name);
}

void CPSLibrary::Remove(const char* nm)
{
    PS::PEDIt it = FindPEDIt(nm);
    if (it != m_PEDs.end())
    {
        (*it)->DestroyShader();
        xr_delete(*it);
        m_PEDs.erase(it);
    }
    else
    {
        PS::PGDIt it2 = FindPGDIt(nm);
        if (it2 != m_PGDs.end())
        {
            xr_delete(*it2);
            m_PGDs.erase(it2);
        }
    }
}
//----------------------------------------------------
bool CPSLibrary::Load2()
{
    FS_FileSet files;
    string_path _path;
    FS.update_path(_path, "$game_particles$", "");

    FS.file_list(files, _path, FS_ListFiles, "*.pe,*.pg");

#ifdef _EDITOR
    SPBItem* pb = nullptr;
    if (UI->m_bReady)
        pb = UI->ProgressStart(files.size(), "Loading particles...");
#endif
    FS_FileSet::iterator it = files.begin();
    FS_FileSet::iterator it_e = files.end();

    string_path p_path, p_name, p_ext;
    for (; it != it_e; ++it)
    {
        const FS_File& f = (*it);
        _splitpath(f.name.c_str(), 0, p_path, p_name, p_ext);
        FS.update_path(_path, "$game_particles$", f.name.c_str());
        CInifile ini(_path, TRUE, TRUE, FALSE);

#ifdef _EDITOR
        if (pb)
            pb->Inc();
#endif

        xr_sprintf(_path, sizeof(_path), "%s%s", p_path, p_name);
        if (0 == xr_stricmp(p_ext, ".pe"))
        {
            PS::CPEDef* def = xr_new<PS::CPEDef>();
            def->m_Name = _path;
            if (def->Load2(ini))
                m_PEDs.push_back(def);
            else
                xr_delete(def);
        }
        else if (0 == xr_stricmp(p_ext, ".pg"))
        {
            PS::CPGDef* def = xr_new<PS::CPGDef>();
            def->m_Name = _path;
            if (def->Load2(ini))
                m_PGDs.push_back(def);
            else
                xr_delete(def);
        }
        else
        {
            R_ASSERT(0);
        }
    }

    std::sort(m_PEDs.begin(), m_PEDs.end(), ped_sort_pred);
    std::sort(m_PGDs.begin(), m_PGDs.end(), pgd_sort_pred);

    for (PS::PEDIt e_it = m_PEDs.begin(); e_it != m_PEDs.end(); ++e_it)
        (*e_it)->CreateShader();

#ifdef _EDITOR
    if (pb)
        UI->ProgressEnd(pb);
#endif
    Msg("Loaded particles :%d", files.size());
    return true;
}

bool CPSLibrary::Load(const char* nm)
{
    if (!FS.exist(nm))
    {
        Msg("Can't find file: '%s'", nm);
        return false;
    }

    ZoneScoped;

    IReader* F = FS.r_open(nm);
    bool bRes = true;
    R_ASSERT(F->find_chunk(PS_CHUNK_VERSION));
    u16 ver = F->r_u16();
    if (ver != PS_VERSION)
        return false;

    // second generation
    IReader* OBJ;
    OBJ = F->open_chunk(PS_CHUNK_SECONDGEN);
    if (OBJ)
    {
        ZoneScopedN("Second generation");
        IReader* O = OBJ->open_chunk(0);
        for (int count = 1; O; count++)
        {
            PS::CPEDef* def = xr_new<PS::CPEDef>();
            if (def->Load(*O))
                m_PEDs.push_back(def);
            else
            {
                bRes = false;
                xr_delete(def);
            }
            O->close();
            if (!bRes)
                break;
            O = OBJ->open_chunk(count);
        }
        OBJ->close();
    }
    // third generation
    OBJ = F->open_chunk(PS_CHUNK_THIRDGEN);
    if (OBJ)
    {
        ZoneScopedN("Third generation");
        IReader* O = OBJ->open_chunk(0);
        for (int count = 1; O; count++)
        {
            PS::CPGDef* def = xr_new<PS::CPGDef>();
            if (def->Load(*O))
                m_PGDs.push_back(def);
            else
            {
                bRes = false;
                xr_delete(def);
            }
            O->close();
            if (!bRes)
                break;
            O = OBJ->open_chunk(count);
        }
        OBJ->close();
    }

    // final
    FS.r_close(F);

    std::sort(m_PEDs.begin(), m_PEDs.end(), ped_sort_pred);
    std::sort(m_PGDs.begin(), m_PGDs.end(), pgd_sort_pred);

    for (PS::PEDIt e_it = m_PEDs.begin(); e_it != m_PEDs.end(); ++e_it)
        (*e_it)->CreateShader();

    return bRes;
}
//----------------------------------------------------
void CPSLibrary::Reload()
{
    OnDestroy();
    OnCreate();
    Msg("PS Library was succesfully reloaded.");
}
//----------------------------------------------------

using PS::CPGDef;

CPGDef const* const* CPSLibrary::particles_group_begin() const { return (m_PGDs.size() ? &m_PGDs.front() : 0); }
CPGDef const* const* CPSLibrary::particles_group_end() const { return (m_PGDs.size() ? &m_PGDs.back() : 0); }
void CPSLibrary::particles_group_next(PS::CPGDef const* const*& iterator) const
{
    VERIFY(iterator);
    VERIFY(iterator >= particles_group_begin());
    VERIFY(iterator < particles_group_end());
    ++iterator;
}

shared_str const& CPSLibrary::particles_group_id(CPGDef const& particles_group) const
{
    return (particles_group.m_Name);
}
} // namespace xray::render::RENDER_NAMESPACE
