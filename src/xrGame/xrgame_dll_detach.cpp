#include "pch_script.h"
#include "ai_space.h"
#include "object_factory.h"
#include "ai/monsters/ai_monster_squad_manager.h"

#include "entity_alive.h"
#include "ui/UIInventoryUtilities.h"
#include "ui/UIXmlInit.h"
#include "xrUICore/XML//UITextureMaster.h"

#include "InfoPortion.h"
#include "PhraseDialog.h"
#include "GameTask.h"
#include "encyclopedia_article.h"

#include "character_info.h"
#include "specific_character.h"
#include "character_community.h"
#include "monster_community.h"
#include "character_rank.h"
#include "character_reputation.h"

#include "xrEngine/profiler.h"

#include "sound_collection_storage.h"
#include "relation_registry.h"
#include "script_properties_list_helper.h"

extern CScriptPropertiesListHelper* g_property_list_helper;

typedef xr_vector<std::pair<shared_str, int>> STORY_PAIRS;
extern STORY_PAIRS story_ids;
extern STORY_PAIRS spawn_story_ids;

extern void release_smart_cast_stats();
extern void InitHudSoundSettings();

#include "xrEngine/IGame_Persistent.h"

// The first template-profile human spawn of a session otherwise pays for loading every specific
// character from XML at once (CSE_ALifeTraderAbstract::specific_character enumerates them all).
// Both tables use auto_delete = false and live until clean_game_globals, so one pass over the
// same public lookup path here, inside the startup task, turns every later lookup into a read.
// Character metadata only - no visuals, sounds or other resources are touched.
static void warmup_character_descriptions()
{
    for (int i = 0, e = CSpecificCharacter::GetMaxIndex(); i <= e; ++i)
    {
        const shared_str id = CSpecificCharacter::IndexToId(i);
        if (!id.size())
            continue;

        CSpecificCharacter character;
        character.Load(id);
    }

    for (int i = 0, e = CCharacterInfo::GetMaxIndex(); i <= e; ++i)
    {
        const shared_str id = CCharacterInfo::IndexToId(i);
        if (!id.size())
            continue;

        CCharacterInfo profile;
        profile.Load(id);
    }
}

void init_game_globals()
{
    ZoneScoped;

    InitHudSoundSettings();
    if (!GEnv.isDedicatedServer)
    {
        CInfoPortion::InitInternal(ShadowOfChernobylMode || ClearSkyMode, true);
        CEncyclopediaArticle::InitInternal(ShadowOfChernobylMode, true);
        CPhraseDialog::InitInternal();
    };
    CCharacterInfo::InitInternal();
    CSpecificCharacter::InitInternal();
    CHARACTER_COMMUNITY::InitInternal();
    CHARACTER_RANK::InitInternal();
    CHARACTER_REPUTATION::InitInternal();
    MONSTER_COMMUNITY::InitInternal();

    warmup_character_descriptions();
}

void clean_game_globals()
{
    xr_delete(g_property_list_helper);

    // destroy ai space
    xr_delete(g_ai_space);
    // destroy object factory
    xr_delete(g_object_factory);
    // destroy monster squad global var
    xr_delete(g_monster_squad);

    story_ids.clear();
    spawn_story_ids.clear();

    if (!GEnv.isDedicatedServer)
    {
        CInfoPortion::DeleteSharedData();
        CInfoPortion::DeleteIdToIndexData();

        CEncyclopediaArticle::DeleteSharedData();
        CEncyclopediaArticle::DeleteIdToIndexData();

        CPhraseDialog::DeleteSharedData();
        CPhraseDialog::DeleteIdToIndexData();
    }
    CCharacterInfo::DeleteSharedData();
    CCharacterInfo::DeleteIdToIndexData();

    CSpecificCharacter::DeleteSharedData();
    CSpecificCharacter::DeleteIdToIndexData();

    CHARACTER_COMMUNITY::DeleteIdToIndexData();
    CHARACTER_RANK::DeleteIdToIndexData();
    CHARACTER_REPUTATION::DeleteIdToIndexData();
    MONSTER_COMMUNITY::DeleteIdToIndexData();

    // static shader for blood
    CEntityAlive::UnloadBloodyWallmarks();
    CEntityAlive::UnloadFireParticles();

    // Очищение таблицы идентификаторов рангов и отношений сталкеров
    InventoryUtilities::ClearCharacterInfoStrings();

    xr_delete(g_sound_collection_storage);

#ifdef DEBUG
    xr_delete(g_profiler);
    release_smart_cast_stats();
#endif

    RELATION_REGISTRY::clear_relation_registry();
}
