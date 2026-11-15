// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

#include "xrCore/xr_types.h"

#include <array>

namespace SaveExtensionChunkIds
{
inline constexpr u32 ActorAdrenaline = 0x31524441;
inline constexpr u32 HelmetFilters = 0x31544648;
inline constexpr u32 ArtefactOverrides = 0x314F4641;
inline constexpr u32 WeaponExtended = 0x31584557;
// WFL1: the weapon fault accumulators (fouling, stress, wear, cleaning-cycle stage).
inline constexpr u32 WeaponFouling = 0x314C4657;
inline constexpr u32 EnvironmentSeason = 0x31414553;
// NQ quest-graph runtime state: an opaque marshal blob owned by the xms_nq Lua
// runtime (xms.save_data("xms.nq", ...)). Lives on the XMS per-module data
// channel 0x584D0000|ns under the core pseudo-namespace 0xFF01; the range
// 0xFF00-0xFFFF is reserved for such engine-owned blobs and is never handed to
// a module by the ns registry.
inline constexpr u32 XmsNqState = 0x584DFF01;
// AIK1: per-NPC beliefs of the human NPC AI (target, estimated position and error, age, source).
inline constexpr u32 AiKnowledge = 0x314B4941;
// AIT1: per-NPC traits rolled once per lifetime (the radio), bound by object id and section CRC.
inline constexpr u32 AiTraits = 0x31544941;
// XMK1: the modules absent from the game that this save keeps content of (id, version, ns, spawn
// range). The module chunk families it names are ranges of their own, by the high word, never
// handed to a fixed id: 0x584D|ns per-module data, 0x584C|ns spawn ledger, 0x584F|ns the module's
// objects, 0x5845|ns the game's objects' state that names the module's data (xms_game.h).
inline constexpr u32 XmsKeptModules = 0x314B4D58;

inline constexpr std::array registered{ActorAdrenaline, HelmetFilters, ArtefactOverrides, WeaponExtended,
    WeaponFouling, EnvironmentSeason, XmsNqState, AiKnowledge, AiTraits, XmsKeptModules};

// No fixed id may fall into a module family.
consteval bool registered_ids_outside_families()
{
    for (const u32 id : registered)
    {
        const u32 family = id & 0xFFFF0000;
        if (id != XmsNqState && (family == 0x584D0000 || family == 0x584C0000 || family == 0x584F0000 ||
            family == 0x58450000))
            return false;
    }
    return true;
}

static_assert(registered_ids_outside_families());

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
