#pragma once

#include "save_extension_container.h"
#include "Actor.h"
#include "ActorHelmet.h"
#include "Artefact.h"
#include "Weapon.h"

namespace SaveExtensionGameplay
{
struct ChunkMutation
{
    SaveExtensionContainer::Chunk chunk;
    u16 newestSupportedVersion{};
    bool erase{};
};

using ChunkMutationList = xr_vector<ChunkMutation>;

struct CaptureState
{
    SWeaponExtendedSaveCaptureState weapon;
    SActorAdrenalineSaveCaptureState actor;
    SHelmetFilterSaveCaptureState helmet;
    SArtefactOverrideSaveCaptureState artefact;
    ChunkMutationList mutations;
    xr_vector<u8> payload;
    size_t encodeIndex{};
    u8 phase{};
    bool initialized{};
    bool completed{};
    bool failed{};
};

void begin_capture(CaptureState& state);
[[nodiscard]] bool continue_capture(CaptureState& state, float budgetMilliseconds);
[[nodiscard]] bool apply_mutations(
    SaveExtensionContainer::ChunkList& chunks, ChunkMutationList mutations);
void stage_chunks(const SaveExtensionContainer::ChunkList& chunks);
void clear_state();
void forget_object(const CSE_Abstract& serverObject);
}
