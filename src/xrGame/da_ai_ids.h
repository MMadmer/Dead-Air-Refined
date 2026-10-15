// Copyright (c) 2026 Grizzly
// Released under the MIT licence, see License.txt

#pragma once

// Planner ids of the human NPC AI. Script ids are offsets from stalker_ids.property_script / action_script and reach
// a few hundred thousand (mods hard-code values like 188113), so this range stays far above anything a script uses.
// The values are serialized by nothing: the new properties are evaluator-backed and the operators save no state.
namespace DaAi::Ids
{
// world properties (plain evaluators)
inline constexpr u32 prop_out_of_band = 0x00DA0001;
inline constexpr u32 prop_suppress_role = 0x00DA0002;
inline constexpr u32 prop_should_fall_back = 0x00DA0003;
inline constexpr u32 prop_return_fire = 0x00DA0004;
inline constexpr u32 prop_contact_lost = 0x00DA0005;
inline constexpr u32 prop_flank_role = 0x00DA0006;
inline constexpr u32 prop_search_spot = 0x00DA0010;
inline constexpr u32 prop_search_cover = 0x00DA0011;

// operators (plain CStalkerActionBase subclasses)
inline constexpr u32 op_suppress_fire = 0x00DA0101;
inline constexpr u32 op_fall_back = 0x00DA0102;
inline constexpr u32 op_hold_ambush = 0x00DA0103;
inline constexpr u32 op_return_fire = 0x00DA0104;
inline constexpr u32 op_search_handover = 0x00DA0105;
inline constexpr u32 op_flank = 0x00DA0106;
inline constexpr u32 op_search_sweep = 0x00DA0110;
inline constexpr u32 op_search_overwatch = 0x00DA0111;
} // namespace DaAi::Ids
