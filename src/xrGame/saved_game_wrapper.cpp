////////////////////////////////////////////////////////////////////////////
//	Module 		: saved_game_wrapper.cpp
//	Created 	: 21.02.2006
//  Modified 	: 21.02.2006
//	Author		: Dmitriy Iassenev
//	Description : saved game wrapper class
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "saved_game_wrapper.h"
#include "alife_time_manager.h"
#include "alife_object_registry.h"
#include "xrServer_Objects_ALife_Monsters.h"
#include "ai_space.h"
#include "xrAICore/Navigation/game_graph.h"
#include "alife_simulator_header.h"
#include "alife_simulator.h"
#include "alife_spawn_registry.h"
#include "alife_storage_manager.h"

#include <future>
#include <mutex>

extern LPCSTR alife_section;

namespace
{
constexpr u32 saveFooterMagic = 0x53343644;
constexpr u32 saveFooterVersion = 1;
constexpr u32 saveProtectedSourceFlag = 0x80000000;
constexpr size_t saveFooterSize = sizeof(u32) * 5 + sizeof(u64);
constexpr size_t maxSaveSourceSize = 512ull * 1024 * 1024;

struct PreparedSaveData
{
    xr_string name;
    xr_vector<u8> source;
    CSavedGameWrapper::SaveMetadata metadata;
    bool valid{};
};

std::mutex preparedSaveMutex;
std::future<PreparedSaveData> preparedSaveFuture;
xr_string preparedSaveName;

bool valid_saved_game_file(pcstr fileName)
{
    IReader* stream = FS.r_open(fileName);
    if (!stream)
        return false;

    const bool valid = CSavedGameWrapper::valid_saved_game(*stream);
    FS.r_close(stream);
    return valid;
}
}

void CSavedGameWrapper::begin_async_load(LPCSTR saved_game_name)
{
    if (!saved_game_name || !saved_game_name[0])
        return;

    std::lock_guard lock(preparedSaveMutex);
    if (preparedSaveFuture.valid())
    {
        if (preparedSaveName == saved_game_name)
            return;
        preparedSaveFuture.wait();
        preparedSaveFuture = {};
    }

    preparedSaveName = saved_game_name;
    const xr_string requestedName = saved_game_name;
    preparedSaveFuture = std::async(std::launch::async, [requestedName]
    {
        PreparedSaveData result;
        result.name = requestedName;

        string_path fileName;
        saved_game_full_name(requestedName.c_str(), fileName, SAVE_EXTENSION);
        if (!FS.exist(fileName))
            saved_game_full_name(requestedName.c_str(), fileName, SAVE_EXTENSION_LEGACY);

        IReader* stream = FS.r_open(fileName);
        if (!stream || stream->length() < 3 * sizeof(u32))
        {
            if (stream)
                FS.r_close(stream);
            return result;
        }

        const u32 marker = stream->r_u32();
        const u32 version = stream->r_u32();
        if (marker != u32(-1) || version < ALIFE_VERSION || !read_metadata(*stream, result.metadata))
        {
            FS.r_close(stream);
            return result;
        }

        stream->seek(3 * sizeof(u32));
        result.source.resize(result.metadata.sourceSize);
        const size_t decodedSize = rtc_decompress(
            result.source.data(), result.source.size(), stream->pointer(), result.metadata.compressedSize);
        FS.r_close(stream);

        result.valid = decodedSize == result.source.size();
        if (!result.valid)
            result.source.clear();
        return result;
    });
}

bool CSavedGameWrapper::consume_async_load(
    LPCSTR saved_game_name, xr_vector<u8>& source_data, SaveMetadata& metadata)
{
    std::future<PreparedSaveData> pending;
    {
        std::lock_guard lock(preparedSaveMutex);
        if (!preparedSaveFuture.valid() || preparedSaveName != saved_game_name)
            return false;
        pending = std::move(preparedSaveFuture);
        preparedSaveName.clear();
    }

    PreparedSaveData prepared = pending.get();
    if (!prepared.valid || prepared.name != saved_game_name)
        return false;

    source_data = std::move(prepared.source);
    metadata = prepared.metadata;
    return true;
}

pcstr CSavedGameWrapper::saved_game_full_name(pcstr saved_game_name, string_path& result, pcstr extension)
{
    string_path temp;
    strconcat(sizeof(temp), temp, saved_game_name, extension);
    FS.update_path(result, "$game_saves$", temp);
    return (result);
}

bool CSavedGameWrapper::saved_game_exist(LPCSTR saved_game_name)
{
    CALifeStorageManager::wait_for_pending_saves();

    string_path file_name;
    if (FS.exist(saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION)))
        return true;
    return FS.exist(saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION_LEGACY));
}

bool CSavedGameWrapper::valid_saved_game(IReader& stream)
{
    if (stream.length() < 3 * sizeof(u32))
        return (false);

    if (stream.r_u32() != u32(-1))
        return (false);

    if (stream.r_u32() < ALIFE_VERSION)
        return (false);

    SaveMetadata metadata;
    if (!read_metadata(stream, metadata))
        return false;

    if (metadata.protectedFormat)
        return true;

    const size_t savedPosition = stream.tell();
    stream.seek(3 * sizeof(u32));
    void* sourceData = xr_malloc(metadata.sourceSize);
    const size_t decodedSize =
        rtc_decompress(sourceData, metadata.sourceSize, stream.pointer(), metadata.compressedSize);
    xr_free(sourceData);
    stream.seek(savedPosition);
    return decodedSize == metadata.sourceSize;
}

bool CSavedGameWrapper::valid_saved_game(LPCSTR saved_game_name)
{
    CALifeStorageManager::wait_for_pending_saves();

    string_path file_name;
    if (!FS.exist(saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION)))
        if (!FS.exist(saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION_LEGACY)))
            return false;

    if (valid_saved_game_file(file_name))
        return true;

    string_path backupName;
    strconcat(sizeof(backupName), backupName, file_name, ".bak");
    if (!FS.exist(backupName) || !valid_saved_game_file(backupName))
        return false;

    string_path recoveryName;
    strconcat(sizeof(recoveryName), recoveryName, file_name, ".recover");
    FS.file_copy(backupName, recoveryName);
    FS.file_rename(recoveryName, file_name, true);

    string_path customName;
    xr_strcpy(customName, file_name);
    if (char* extension = strrchr(customName, '.'))
        xr_strcpy(extension, sizeof(customName) - (extension - customName), ".scoc");

    string_path customBackup;
    strconcat(sizeof(customBackup), customBackup, customName, ".bak");
    if (FS.exist(customBackup))
    {
        string_path customRecovery;
        strconcat(sizeof(customRecovery), customRecovery, customName, ".recover");
        FS.file_copy(customBackup, customRecovery);
        FS.file_rename(customRecovery, customName, true);
    }

    Msg("! Restored the previous complete save pair for '%s'", saved_game_name);
    return valid_saved_game_file(file_name);
}

bool CSavedGameWrapper::read_metadata(IReader& stream, SaveMetadata& metadata)
{
    const size_t savedPosition = stream.tell();
    const size_t streamLength = stream.length();
    metadata = {};

    if (streamLength < 3 * sizeof(u32))
        return false;

    stream.seek(2 * sizeof(u32));
    const u32 sourceMarker = stream.r_u32();
    const bool protectedHeader = (sourceMarker & saveProtectedSourceFlag) != 0;
    const u32 sourceSize = sourceMarker & ~saveProtectedSourceFlag;
    metadata.sourceSize = sourceSize;
    metadata.compressedSize = streamLength - 3 * sizeof(u32);

    if (streamLength >= 3 * sizeof(u32) + saveFooterSize)
    {
        stream.seek(streamLength - saveFooterSize);
        const u32 magic = stream.r_u32();
        if (magic == saveFooterMagic)
        {
            const u32 formatVersion = stream.r_u32();
            metadata.saveId = stream.r_u64();
            const u32 footerSourceSize = stream.r_u32();
            const u32 footerCompressedSize = stream.r_u32();
            const u32 footerChecksum = stream.r_u32();

            metadata.protectedFormat = true;
            metadata.compressedSize = streamLength - 3 * sizeof(u32) - saveFooterSize;

            if (formatVersion != saveFooterVersion || footerSourceSize != sourceSize ||
                footerCompressedSize != metadata.compressedSize)
            {
                stream.seek(savedPosition);
                return false;
            }

            stream.seek(3 * sizeof(u32));
            if (crc32(stream.pointer(), static_cast<u32>(metadata.compressedSize)) != footerChecksum)
            {
                stream.seek(savedPosition);
                return false;
            }
        }
    }

    if (protectedHeader && !metadata.protectedFormat)
    {
        stream.seek(savedPosition);
        return false;
    }

    stream.seek(savedPosition);
    return metadata.compressedSize > 0 && metadata.sourceSize > 0 && metadata.sourceSize <= maxSaveSourceSize;
}

CSavedGameWrapper::CSavedGameWrapper(LPCSTR saved_game_name)
{
    string_path file_name;
    saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION);
    if (!FS.exist(file_name))
        saved_game_full_name(saved_game_name, file_name, SAVE_EXTENSION_LEGACY);

    R_ASSERT3(FS.exist(file_name), "There is no saved game ", saved_game_name);

    IReader* stream = FS.r_open(file_name);
    if (!valid_saved_game(*stream))
    {
        FS.r_close(stream);
        CALifeTimeManager time_manager(alife_section);
        m_game_time = time_manager.game_time();
        m_actor_health = 1.f;
        m_level_id = _LEVEL_ID(-1);
        m_level_name = "";
        return;
    }

    SaveMetadata metadata;
    if (!read_metadata(*stream, metadata))
    {
        FS.r_close(stream);
        m_level_id = _LEVEL_ID(-1);
        m_level_name = "";
        return;
    }
    stream->advance(sizeof(u32));
    const u32 source_count = static_cast<u32>(metadata.sourceSize);
    void* source_data = xr_malloc(source_count);
    const size_t decodedSize = rtc_decompress(source_data, source_count, stream->pointer(), metadata.compressedSize);
    FS.r_close(stream);
    if (decodedSize != source_count)
    {
        xr_free(source_data);
        m_level_id = _LEVEL_ID(-1);
        m_level_name = "";
        return;
    }

    IReader reader(source_data, source_count);

    {
        CALifeTimeManager time_manager(alife_section);
        time_manager.load(reader);
        m_game_time = time_manager.game_time();
    }

    {
        R_ASSERT2(reader.find_chunk(OBJECT_CHUNK_DATA), "Can't find chunk OBJECT_CHUNK_DATA!");
        [[maybe_unused]] u32 count = reader.r_u32();
        VERIFY(count > 0);
        CSE_ALifeDynamicObject* object = CALifeObjectRegistry::get_object(reader);
        VERIFY(object->ID == 0);
        CSE_ALifeCreatureActor* actor = smart_cast<CSE_ALifeCreatureActor*>(object);
        VERIFY(actor);

        m_actor_health = actor->get_health();

        IReader* chunk = reader.open_chunk(SPAWN_CHUNK_DATA);
        R_ASSERT2(chunk, "Spawn version mismatch - REBUILD SPAWN!");

        string_path spawn_file_name;
        {
            IReader* sub_chunk = chunk->open_chunk(0);
            if (!sub_chunk)
            {
                chunk->close();
                F_entity_Destroy(object);
                m_level_id = _LEVEL_ID(-1);
                m_level_name = "";
                return;
            }
            sub_chunk->r_stringZ(spawn_file_name, sizeof(spawn_file_name));
            sub_chunk->close();
        }

        chunk->close();

        if (!FS.exist(file_name, "$game_spawn$", spawn_file_name, ".spawn"))
        {
            F_entity_Destroy(object);
            m_level_id = _LEVEL_ID(-1);
            m_level_name = "";
            return;
        }

        IReader* spawn = NULL;
        bool b_destroy_spawn = true;
        if (ai().get_alife() && ai().alife().spawns().get_spawn_name() == spawn_file_name)
        {
            spawn = ai().alife().spawns().get_spawn_file();
            b_destroy_spawn = false;
        }
        else
            spawn = FS.r_open(file_name);

        if (!spawn)
        {
            F_entity_Destroy(object);
            m_level_id = _LEVEL_ID(-1);
            m_level_name = "";
            return;
        }

        chunk = spawn->open_chunk(4);
        if (!chunk) // Shadow of Chernobyl
        {
            string_path graph_path;
            FS.update_path(graph_path, "$game_data$", GRAPH_NAME);
            chunk = FS.r_open(graph_path);
        }

        if (!chunk)
        {
            F_entity_Destroy(object);
            if (b_destroy_spawn)
                FS.r_close(spawn);
            m_level_id = _LEVEL_ID(-1);
            m_level_name = "";
            return;
        }

        {
            CGameGraph graph(*chunk);
            m_level_id = graph.vertex(object->m_tGraphID)->level_id();
            if (graph.header().level_exist(m_level_id))
                m_level_name = graph.header().level(m_level_id).name();
            else
                m_level_name = StringTable().translate("ui_st_error");
        }

        chunk->close();
        if (b_destroy_spawn)
            FS.r_close(spawn);
        F_entity_Destroy(object);
    }

    xr_free(source_data);
}
