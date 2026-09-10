#pragma once

// [DA] An artefact inside a container, for the code that asks whether the actor HAS one.
//
// A container in this game is not a box with something in it. Putting an artefact into one
// destroys both objects and creates a single new object whose section is the two names joined:
// af_medusa in a lead_box becomes one item of section "af_medusa_lead_box". That is why a quest
// that asks for "af_medusa" comes up empty while the artefact is plainly in the rucksack - the
// object is right there in the inventory, and its section is simply a different string.
//
// So the whole of this is a NAME RULE: the section "<artefact>_<container>" is that artefact,
// for the purpose of answering the question. Nothing is unpacked, nothing is created, the
// containerised artefact keeps its shielding, and the field that a quest cannot see it through
// is the only thing that changes. Taking it is a separate matter and belongs to the game side,
// where a container can be emptied and handed back - see dead_air_x64_af_container.script.
//
// The rucksack only. A container on the belt is being WORN, and the artefact in it is doing its
// work there; a quest may not reach into it any more than it may take the boots off your feet.
//
// No container's name is written here. The sections come from data, and the data says where the
// installed container mod keeps its own list - so a mod that adds a fifth container type is
// covered by the fact that it registered it with the mod it extends.

class CInventory;
class CInventoryItem;

namespace da_af_container
{
// Is the rule on at all? Data, so a setup that does not want it can say so.
bool quest_visible();

// The artefact a filled container holds, or nullptr when this section is not a filled container.
// The result points into a caller-owned buffer.
pcstr held_artefact(pcstr section, string256& buffer);

// The container in this inventory's RUCKSACK standing in for `wanted`, or nullptr. The caller
// looks for the artefact itself first: a loose one always wins, and only when there is none does
// a container answer for it.
CInventoryItem* find_in_ruck(const CInventory& inventory, pcstr wanted);
} // namespace da_af_container
