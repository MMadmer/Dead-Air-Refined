#include "stdafx.h"
#include "StringTable.h"

#include "xr_level_controller.h"

#include "xrCore/XML/XMLDocument.hpp"
#include "xrCore/Threading/ParallelForEach.hpp"

constexpr pcstr OPENXRAY_XML = "openxray.xml";

namespace
{
constexpr u32 STRING_TABLE_CACHE_MAGIC = 0x31435453;
constexpr u32 STRING_TABLE_CACHE_VERSION = 2;
constexpr u32 MAX_CACHED_STRINGS = 1'000'000;
constexpr size_t MAX_CACHED_STRING_LENGTH = 1024 * 1024;

struct LocalizationFile
{
    xr_string name;
    STRING_TABLE_MAP strings;
};

u32 CalculateCacheSignature(const FS_FileSet& files, pcstr language)
{
    u32 signature = crc32(&STRING_TABLE_CACHE_VERSION, sizeof(STRING_TABLE_CACHE_VERSION));
    signature = crc32(language, xr_strlen(language), signature);
    for (const FS_File& file : files)
    {
        signature = crc32(file.name.data(), file.name.size(), signature);
        signature = crc32(&file.size, sizeof(file.size), signature);
        signature = crc32(&file.time_write, sizeof(file.time_write), signature);
    }
    return signature;
}

bool ReadCachedString(IReader& reader, xr_string& value)
{
    if (reader.eof())
        return false;

    const void* terminator = std::memchr(reader.pointer(), 0, reader.elapsed());
    if (!terminator)
        return false;

    const size_t length = static_cast<const u8*>(terminator) - static_cast<const u8*>(reader.pointer());
    if (length > MAX_CACHED_STRING_LENGTH)
        return false;

    reader.r_stringZ(value);
    return true;
}

}

CStringTable& StringTable()
{
    static CStringTable string_table;
    return string_table;
}

xr_unique_ptr<STRING_TABLE_DATA> CStringTable::pData{};
u32 CStringTable::LanguageID = std::numeric_limits<u32>::max();
string32 CStringTable::LanguageIDInLTX{};
xr_vector<xr_token> CStringTable::languagesToken;

void CStringTable::Destroy()
{
    pData.reset(nullptr);

    for (auto& token : languagesToken)
    {
        auto tokenName = const_cast<char*>(token.name);
        xr_free(tokenName);
    }

    languagesToken.clear();
}

void CStringTable::rescan()
{
    if (!pData)
    {
        Destroy();
        Init();
    }
}

void CStringTable::Init()
{
    if (pData)
        return;

    ZoneScoped;

    pData = xr_make_unique<STRING_TABLE_DATA>();

    FillLanguageToken();
    SetLanguage();

    FS_FileSet fset;
    string_path files_mask;
    xr_sprintf(files_mask, "text" DELIMITER "%s" DELIMITER "*.xml", pData->m_sLanguage.c_str());
    FS.file_list(fset, "$game_config$", FS_ListFiles, files_mask);

    string_path cacheName;
    xr_sprintf(cacheName, "string-table-%s.cache", pData->m_sLanguage.c_str());
    string_path cachePath;
    FS.update_path(cachePath, "$app_data_root$", cacheName);
    const u32 signature = CalculateCacheSignature(fset, pData->m_sLanguage.c_str());

    if (!LoadCache(cachePath, signature, pData->m_StringTable))
    {
        xr_vector<LocalizationFile> files;
        files.reserve(fset.size());
        for (const FS_File& file : fset)
        {
            string_path name, extension;
            _splitpath(file.name.c_str(), nullptr, nullptr, name, extension);
            xr_strcat(name, extension);
            files.push_back({ name, {} });
        }

        xr_parallel_for_each(files, [this](LocalizationFile& file)
        {
            Load(file.name.c_str(), file.strings);
        });

        for (LocalizationFile& file : files)
        {
            for (const auto& [id, value] : file.strings)
            {
#ifndef MASTER_GOLD
                if (pData->m_StringTable.find(id) != pData->m_StringTable.end())
                    Msg("* String table override [%s]", id.c_str());
#endif
                pData->m_StringTable[id] = value;
            }
        }
        SaveCache(cachePath, signature, pData->m_StringTable);
    }

    if (!translate("st_currency", pData->m_sCurrency) &&
        !translate("ui_st_money_descr", pData->m_sCurrency) && // OGSR
        !translate("ui_st_money_regional", pData->m_sCurrency)) // xp-dev
    {
        pData->m_sCurrency = pSettingsOpenXRay->read_if_exists<pcstr>("gameplay", "currency", "RU");
    }

#ifndef MASTER_GOLD
    Msg("StringTable: loaded %d files", fset.size());
#endif
}

void CStringTable::FillLanguageToken()
{
    ZoneScoped;

    languagesToken.clear();

    string_path path;
    FS.update_path(path, _game_config_, "text" DELIMITER);
    auto languages = FS.file_list_open(path, FS_ListFolders | FS_RootOnly);

    const bool localizationPresent = languages != nullptr;

    // We must warn about lack of localization
    // However we can work without it
    VERIFY(localizationPresent);
    if (localizationPresent)
    {
        int i = 0;
        for (const auto& language : *languages)
        {
            const auto pos = strchr(language, _DELIMITER);
            *pos = '\0'; // we don't need that backslash in the end

            // Skip map_desc folder
            if (0 == xr_strcmp(language, "map_desc"))
                continue;

            bool shouldSkip = false;

            // Open current language folder
            string_path folder;
            strconcat(sizeof(folder), folder, path, language, DELIMITER);
            auto files = FS.file_list_open(folder, FS_ListFiles | FS_RootOnly);

            // Skip empty folder
            if (!files || files->empty())
                shouldSkip = true;

            // Skip folder with only openxray.xml file in it
            // It's important to have 'else if' instead of simple 'if'
            else if (files->size() == 1 && xr_strcmp(files->at(0), OPENXRAY_XML) == 0)
                shouldSkip = true;

            // Don't forget to close opened folder
            FS.file_list_close(files);

            if (shouldSkip)
                continue;

            // Finally, we can add language
            languagesToken.emplace_back(xr_strdup(language), i++); // It's important to have postfix increment!
        }
        FS.file_list_close(languages);
    }

    languagesToken.emplace_back(nullptr, -1);
}

void CStringTable::SetLanguage()
{
    cpcstr defined_language = pSettings->r_string("string_table", "language");
    cpcstr defined_prefix   = pSettings->r_string("string_table", "font_prefix");

    // Before introducing a console command to change the language,
    // mods used to change localization.ltx. We detect that
    // by saving the last value in LanguageIDInLTX
    if (LanguageID != std::numeric_limits<u32>::max() && xr_strcmp(LanguageIDInLTX, defined_language) == 0)
    {
        pData->m_sLanguage = languagesToken.at(LanguageID).name;

        if (0 == xr_strcmp(pData->m_sLanguage, defined_language))
            pData->m_fontPrefix = defined_prefix;
        else
        {
            pData->m_fontPrefix = nullptr;

            constexpr std::tuple<pcstr, pcstr> known_prefixes[] =
            {
                { "fra", "_west" },
                { "ger", "_west" },
                { "ita", "_west" },
                { "spa", "_west" },
                { "pol", "_cent" },
                { "cze", "_cent" },
            };
            for (const auto [language, prefix] : known_prefixes)
            {
                if (0 == xr_strcmp(pData->m_sLanguage, language))
                    pData->m_fontPrefix = prefix;
            }
        }
    }
    else
    {
        pData->m_sLanguage  = defined_language;
        pData->m_fontPrefix = defined_prefix;

        const auto it = std::find_if(languagesToken.begin(), languagesToken.end(), [](const xr_token& token)
        {
            return token.name && token.name == pData->m_sLanguage;
        });

        R_ASSERT3(it != languagesToken.end(), "Check localization.ltx! Current language: ", pData->m_sLanguage.c_str());
        if (it != languagesToken.end())
            LanguageID = it->id;
    }
    xr_strcpy(LanguageIDInLTX, defined_language);
}

shared_str CStringTable::GetCurrentLanguage() const
{
    return pData ? pData->m_sLanguage : nullptr;
}

shared_str CStringTable::GetCurrentFontPrefix() const
{
    return pData ? pData->m_fontPrefix : nullptr;
}

shared_str CStringTable::GetCurrency() const
{
    return pData ? pData->m_sCurrency : "RU";
}

xr_token* CStringTable::GetLanguagesToken() const { return languagesToken.data(); }

void CStringTable::Load(LPCSTR xml_file_full, STRING_TABLE_MAP& strings)
{
    ZoneScoped;

    XMLDocument uiXml;
    string_path _s;
    strconcat(sizeof(_s), _s, "text" DELIMITER, pData->m_sLanguage.c_str());

    uiXml.Load(CONFIG_PATH, _s, xml_file_full);

    //общий список всех записей таблицы в файле
    const size_t string_num = uiXml.GetNodesNum(uiXml.GetRoot(), "string");

    for (size_t i = 0; i < string_num; ++i)
    {
        LPCSTR string_name = uiXml.ReadAttrib(uiXml.GetRoot(), "string", i, "id", NULL);
        LPCSTR string_text = uiXml.Read(uiXml.GetRoot(), "string:text", i, NULL);

        if (!string_text)
        {
#ifndef MASTER_GOLD
            Msg("! [%s] string table entry[%s] doesn't have a translation (no 'text' tag)", xml_file_full, string_name);
#endif
            continue;
        }

        const STRING_VALUE str_val = ParseLine(string_text); // NOLINT
#ifndef MASTER_GOLD
        if (strings.find(string_name) != strings.end())
            Msg("* String table override [%s]", string_name);
#endif
        strings[string_name] = str_val;
    }
}

bool CStringTable::LoadCache(pcstr cachePath, u32 signature, STRING_TABLE_MAP& strings)
{
    IReader* reader = FS.r_open(cachePath);
    if (!reader)
        return false;

    STRING_TABLE_MAP cachedStrings;
    bool valid = reader->elapsed() >= static_cast<intptr_t>(sizeof(u32) * 4);
    u32 count = 0;
    if (valid)
    {
        valid = reader->r_u32() == STRING_TABLE_CACHE_MAGIC;
        valid = valid && reader->r_u32() == STRING_TABLE_CACHE_VERSION;
        valid = valid && reader->r_u32() == signature;
        count = reader->r_u32();
        valid = valid && count <= MAX_CACHED_STRINGS;
    }

    for (u32 i = 0; valid && i < count; ++i)
    {
        xr_string id;
        xr_string value;
        valid = ReadCachedString(*reader, id) && ReadCachedString(*reader, value);
        if (valid)
            valid = cachedStrings.emplace(id.c_str(), value.c_str()).second;
    }
    valid = valid && reader->eof();
    FS.r_close(reader);

    if (!valid)
        return false;

    strings.swap(cachedStrings);
#ifndef MASTER_GOLD
    Msg("StringTable: loaded cache with %u strings", count);
#endif
    return true;
}

void CStringTable::SaveCache(pcstr cachePath, u32 signature, const STRING_TABLE_MAP& strings)
{
    CMemoryWriter writer;
    writer.w_u32(STRING_TABLE_CACHE_MAGIC);
    writer.w_u32(STRING_TABLE_CACHE_VERSION);
    writer.w_u32(signature);
    writer.w_u32(static_cast<u32>(strings.size()));
    for (const auto& [id, value] : strings)
    {
        writer.w_stringZ(id);
        writer.w_stringZ(value);
    }

    xr_string temporaryPath = cachePath;
    temporaryPath += ".tmp";
    if (writer.save_to(temporaryPath.c_str()))
    {
        FS.file_rename(temporaryPath.c_str(), cachePath, true);
    }
    else
    {
#ifndef MASTER_GOLD
        Msg("! StringTable: failed to update cache [%s]", cachePath);
#endif
    }
}

void CStringTable::ReloadLanguage()
{
    if (0 == xr_strcmp(languagesToken.at(LanguageID).name, pData->m_sLanguage.c_str()))
        return;

    Destroy();
    Init();
}

STRING_VALUE CStringTable::ParseLine(pcstr str)
{
    ZoneScoped;

    constexpr char   ACTION_STR[]     = "$$ACTION_";
    constexpr size_t ACTION_STR_LEN   = std::size(ACTION_STR) - 1;

    constexpr char   ACTION_STR_END[] = "$$";
    constexpr size_t ACTION_STR_END_LEN = std::size(ACTION_STR_END) - 1;

    xr_string string{ str };
    string.erase(std::remove_if(string.begin(), string.end(), [](char ch)
    {
        VERIFY2(ch != GAME_ACTION_MARK,"Using of escape symbol is not allowed in localization.");
        return ch == GAME_ACTION_MARK;
    }), string.end());

    size_t actionStartPos = 0;

    while ((actionStartPos = string.find(ACTION_STR, actionStartPos)) != xr_string::npos)
    {
        const size_t actionEndPos = string.find(ACTION_STR_END, actionStartPos + ACTION_STR_LEN);
        const xr_string actionName = string.substr(actionStartPos + ACTION_STR_LEN, actionEndPos - (actionStartPos + ACTION_STR_LEN));

        if (const auto action = ActionNameToPtr(actionName.c_str())) // if exist, get bindings
        {
            static_assert(kLASTACTION < type_max<u8>, "Modify the code to have more than 255 actions.");
            const char actionCode[] = { GAME_ACTION_MARK, (char)action->id, '\0' };
            string.replace(actionStartPos, actionEndPos + ACTION_STR_END_LEN - actionStartPos, actionCode);
            actionStartPos += std::size(actionCode) - 1;
        }
        else // skip
        {
            actionStartPos = actionEndPos + ACTION_STR_END_LEN;
        }
    }

    return { string.c_str() };
}

STRING_VALUE CStringTable::translate(const STRING_ID& str_id) const
{
    if (!pData)
        return str_id;

    if (pData->m_StringTable.find(str_id) != pData->m_StringTable.end())
        return pData->m_StringTable[str_id];
    return str_id;
}

STRING_VALUE CStringTable::translate(const STRING_ID& str_id, const STRING_ID& str_id2) const
{
    STRING_VALUE out = str_id;
    if (!translate(str_id, out))
        translate(str_id2, out);
    return out;
}

bool CStringTable::translate(const STRING_ID& str_id, STRING_VALUE& out) const
{
    if (!pData)
        return false;

    if (pData->m_StringTable.find(str_id) != pData->m_StringTable.end())
    {
        out = pData->m_StringTable[str_id];
        return true;
    }
    return false;
}

bool CStringTable::has_translation(const STRING_ID& str_id) const
{
    if (!pData)
        return false;

    return pData->m_StringTable.find(str_id) != pData->m_StringTable.end();
}
