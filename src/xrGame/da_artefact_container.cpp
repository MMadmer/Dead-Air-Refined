#include "StdAfx.h"
#include "da_artefact_container.h"

#include "Inventory.h"
#include "inventory_item.h"

namespace
{
constexpr pcstr cfg_file = "dead_air_x64_af_container.ltx";
constexpr pcstr cfg_section = "artefact_containers";

bool g_loaded = false;
bool g_visible = false;
xr_vector<shared_str> g_containers;

void add_container(pcstr name)
{
    if (!name || !name[0])
        return;
    for (const shared_str& have : g_containers)
        if (0 == xr_strcmp(have.c_str(), name))
            return;
    g_containers.emplace_back(name);
}

// Every key of a section, as a list of names. Both the mod's own registry and ours are written
// that way - one section per line, no values.
void add_keys_of(const CInifile& ini, pcstr section)
{
    if (!ini.section_exist(section))
        return;
    for (const auto& item : ini.r_section(section).Data)
        add_container(item.first.c_str());
}

void load()
{
    g_loaded = true;

    string_path path;
    FS.update_path(path, "$game_config$", cfg_file);
    if (!FS.exist(path))
        return;

    const CInifile ini(path, TRUE);
    if (!ini.section_exist(cfg_section))
        return;

    g_visible = ini.line_exist(cfg_section, "quest_visible") && ini.r_bool(cfg_section, "quest_visible");
    if (!g_visible)
        return;

    // Where the installed container mod keeps its own list. The file's name and the section's
    // name are data on purpose: this is somebody else's mod, and nothing of it is written here.
    // A mod that adds a container type registers it there to work at all, so it arrives free.
    if (ini.line_exist(cfg_section, "source_file") && ini.line_exist(cfg_section, "source_section"))
    {
        string_path source;
        FS.update_path(source, "$game_config$", ini.r_string(cfg_section, "source_file"));
        if (FS.exist(source))
        {
            const CInifile from_mod(source, TRUE);
            add_keys_of(from_mod, ini.r_string(cfg_section, "source_section"));
        }
    }

    // Anything that registry does not cover, listed by hand.
    add_keys_of(ini, "artefact_containers_extra");
}

const xr_vector<shared_str>& containers()
{
    if (!g_loaded)
        load();
    return g_containers;
}
} // namespace

bool da_af_container::quest_visible()
{
    if (!g_loaded)
        load();
    return g_visible && !g_containers.empty();
}

pcstr da_af_container::held_artefact(pcstr section, string256& buffer)
{
    if (!section || !quest_visible())
        return nullptr;

    const size_t whole = xr_strlen(section);
    for (const shared_str& container : containers())
    {
        // "<artefact>_<container>", and the artefact half has to be a section in its own right:
        // the tail alone would also match a container whose own name happens to end that way.
        const size_t tail = xr_strlen(container.c_str()) + 1;
        if (whole <= tail || whole - tail >= sizeof(buffer))
            continue;
        if (section[whole - tail] != '_' || 0 != xr_strcmp(section + whole - tail + 1, container.c_str()))
            continue;

        xr_strcpy(buffer, sizeof(buffer), section);
        buffer[whole - tail] = 0;
        if (pSettings->section_exist(buffer))
            return buffer;
    }
    return nullptr;
}

CInventoryItem* da_af_container::find_in_ruck(const CInventory& inventory, pcstr wanted)
{
    if (!wanted || !wanted[0] || !quest_visible())
        return nullptr;

    string256 composed;
    for (const shared_str& container : containers())
    {
        xr_sprintf(composed, sizeof(composed), "%s_%s", wanted, container.c_str());
        // Nothing in the world is that section, so no item can be it either.
        if (!pSettings->section_exist(composed))
            continue;

        for (CInventoryItem* item : inventory.m_ruck)
            if (item && 0 == xr_strcmp(item->object().cNameSect().c_str(), composed))
                return item;
    }
    return nullptr;
}
