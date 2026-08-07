////////////////////////////////////////////////////////////////////////////
//	Module 		: saved_game_wrapper.h
//	Created 	: 21.02.2006
//  Modified 	: 21.02.2006
//	Author		: Dmitriy Iassenev
//	Description : saved game wrapper class
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "alife_space.h"
#include "xrAICore/Navigation/game_graph_space.h"

#include <memory>

class CSavedGameWrapper
{
public:
    typedef ALife::_TIME_ID _TIME_ID;
    typedef GameGraph::_LEVEL_ID _LEVEL_ID;
    static constexpr size_t maxSourceSize = 512ull * 1024 * 1024;

    struct SaveMetadata
    {
        size_t sourceSize{};
        size_t compressedSize{};
        u64 saveId{};
        bool protectedFormat{};
    };

    struct PreparedCompanion
    {
        const u8* data{};
        size_t size{};
        bool present{};
    };

    struct PreparedSource
    {
        const u8* data{};
        size_t size{};
        pcstr path{};
        PreparedCompanion custom;
        PreparedCompanion customBackup;
        PreparedCompanion extensions;
        std::shared_ptr<const void> lifetime;

        explicit operator bool() const { return data && size && path && lifetime; }
    };

    enum class PreparedLoadResult
    {
        Unavailable,
        Invalid,
        Ready
    };

private:
    _TIME_ID m_game_time;
    _LEVEL_ID m_level_id;
    shared_str m_level_name;
    float m_actor_health;

public:
    CSavedGameWrapper(LPCSTR saved_game_name);
    static pcstr saved_game_full_name(pcstr saved_game_name, string_path& result, pcstr extension);
    static bool saved_game_exist(LPCSTR saved_game_name);
    static bool valid_saved_game(IReader& stream);
    static bool valid_saved_game(LPCSTR saved_game_name);
    static bool recover_interrupted_save_file_for_commit(LPCSTR main_name);
    static bool recover_interrupted_save(LPCSTR saved_game_name);
    static bool recover_interrupted_transactions();
    static bool read_metadata(IReader& stream, SaveMetadata& metadata);
    static void begin_async_load(LPCSTR saved_game_name);
    static bool consume_async_load(
        LPCSTR saved_game_name, xr_vector<u8>& source_data, SaveMetadata& metadata);
    static bool consume_async_load(LPCSTR saved_game_name, xr_vector<u8>& source_data,
        SaveMetadata& metadata, u64& source_file_size, u32& source_file_checksum);
    static bool consume_async_load(LPCSTR saved_game_name, PreparedSource& source_data,
        SaveMetadata& metadata, u64& source_file_size, u32& source_file_checksum);
    static PreparedLoadResult consume_prepared_load(LPCSTR saved_game_name, PreparedSource& source_data,
        SaveMetadata& metadata, u64& source_file_size, u32& source_file_checksum);
    inline const _TIME_ID& game_time() const;
    inline const _LEVEL_ID& level_id() const;
    inline LPCSTR level_name() const;
    inline const float& actor_health() const;

private:
    DECLARE_SCRIPT_REGISTER_FUNCTION();
};

#include "saved_game_wrapper_inline.h"
