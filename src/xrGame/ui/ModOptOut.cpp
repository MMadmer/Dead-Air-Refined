#include "StdAfx.h"
#include "ModOptOut.h"

namespace
{
xr_vector<shared_str> g_disabling_mods;

// The section a mod (or an XMS .ltxp patch) writes into. Every line is one mod: the key
// keeps declarations from different mods apart, the value is the name shown to the player.
constexpr pcstr kOptOutSection = "auto_update_opt_out";

// Standalone file for addons that do not ship an XMS module.
constexpr pcstr kOptOutFile = "dead_air_x64_mod_opt_out.ltx";

bool already_listed(pcstr name)
{
    for (const shared_str& listed : g_disabling_mods)
        if (xr_strcmp(listed.c_str(), name) == 0)
            return true;
    return false;
}
} // namespace

const xr_vector<shared_str>& ModOptOut::DisablingMods() { return g_disabling_mods; }

bool ModOptOut::AutoUpdateDisabled() { return !g_disabling_mods.empty(); }

void ModOptOut::DisableAutoUpdate(pcstr modName)
{
    if (!modName)
        return;

    // Trim: a name of spaces would render as an empty entry in the list.
    string256 trimmed;
    xr_strcpy(trimmed, modName);
    pstr begin = trimmed;
    while (*begin && isspace(static_cast<unsigned char>(*begin)))
        ++begin;
    pstr end = begin + xr_strlen(begin);
    while (end > begin && isspace(static_cast<unsigned char>(end[-1])))
        --end;
    *end = 0;

    // The ini parser collapses unquoted whitespace, so a name with spaces has to be
    // written quoted in LTX; strip the quotes here the way r_string does.
    if (begin[0] == '"')
    {
        ++begin;
        pstr last = begin + xr_strlen(begin);
        if (last > begin && last[-1] == '"')
            last[-1] = 0;
    }

    if (!begin[0])
    {
        Msg("! [ModOptOut] a mod asked to disable auto-update without giving a name - refused");
        return;
    }

    if (already_listed(begin))
        return;

    g_disabling_mods.emplace_back(begin);
    Msg("* [ModOptOut] auto-update and bug reports disabled by mod '%s'", begin);
}

namespace
{
void load_section(const CInifile& ini)
{
    if (!ini.section_exist(kOptOutSection))
        return;

    for (const auto& [key, value] : ini.r_section(kOptOutSection).Data)
    {
        // `mod_key = Display Name`; a bare key with no value declares itself by key.
        pcstr name = value.size() ? value.c_str() : key.c_str();
        ModOptOut::DisableAutoUpdate(name);
    }
}
} // namespace

void ModOptOut::LoadFromConfig()
{
    // Two sources, because mods reach configs in two different ways: an XMS module
    // patches the shared section through .ltxp without touching anyone else's lines,
    // while a plain addon just drops its own file into configs.
    if (pSettings)
        load_section(*pSettings);

    string_path path;
    FS.update_path(path, CONFIG_PATH, kOptOutFile);
    if (FS.exist(path))
    {
        CInifile ini(path, true, true, false);
        load_section(ini);
    }
}
