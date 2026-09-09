#include "stdafx.h"
#include "da_particle_suppress.h"

static xr_vector<shared_str> s_suppressed;

void da_particle_suppress_set(pcstr names)
{
    s_suppressed.clear();
    if (names && *names)
    {
        const int n = _GetItemCount(names, ',');
        for (int i = 0; i < n; ++i)
        {
            string256 item;
            _GetItem(names, i, item, ',');
            _Trim(item);
            if (*item)
                s_suppressed.emplace_back(item);
        }
    }
    if (!s_suppressed.empty())
        Msg("* [particles] %u effect(s) suppressed by data", u32(s_suppressed.size()));
}

bool da_particle_suppressed(pcstr name)
{
    if (!name || s_suppressed.empty())
        return false;
    for (const auto& s : s_suppressed)
        if (!xr_stricmp(s.c_str(), name))
            return true;
    return false;
}
