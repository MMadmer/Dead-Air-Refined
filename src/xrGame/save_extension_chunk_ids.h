#pragma once

#include "xrCore/xr_types.h"

#include <array>

namespace SaveExtensionChunkIds
{
inline constexpr u32 ActorAdrenaline = 0x31524441;
inline constexpr u32 HelmetFilters = 0x31544648;
inline constexpr u32 ArtefactOverrides = 0x314F4641;
inline constexpr u32 WeaponExtended = 0x31584557;
inline constexpr u32 EnvironmentSeason = 0x31414553;
// NQ quest-graph runtime state: an opaque marshal blob owned by the xms_nq Lua
// runtime (xms.save_data("xms.nq", ...)). Lives on the XMS per-module data
// channel 0x584D0000|ns under the core pseudo-namespace 0xFF01; the range
// 0xFF00-0xFFFF is reserved for such engine-owned blobs and is never handed to
// a module by the ns registry.
inline constexpr u32 XmsNqState = 0x584DFF01;

inline constexpr std::array registered{
    ActorAdrenaline, HelmetFilters, ArtefactOverrides, WeaponExtended, EnvironmentSeason, XmsNqState};

consteval bool registered_ids_are_unique()
{
    for (size_t left = 0; left < registered.size(); ++left)
    {
        for (size_t right = left + 1; right < registered.size(); ++right)
        {
            if (registered[left] == registered[right])
                return false;
        }
    }
    return true;
}

static_assert(registered_ids_are_unique());
}
