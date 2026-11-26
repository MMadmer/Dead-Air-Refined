#include "stdafx.h"
#include "StringTable.h"

#include "xr_level_controller.h"

#include "xrCommon/xr_hash_map.h"
#include "xrCore/XML/XMLDocument.hpp"
#include "xrCore/XMS/xms_core.h"
#include "xrCore/XMS/xms_names.h"

// The engine's own string file, under the name it has now and the one it had as OpenXRay.
constexpr pcstr ENGINE_XML[] = { "grizzly.xml", "openxray.xml" };
static bool is_engine_xml(pcstr name)
{
    for (pcstr n : ENGINE_XML)
        if (!xr_stricmp(name, n))
            return true;
    return false;
}

namespace
{
constexpr u32 STRING_TABLE_CACHE_MAGIC = 0x31435453;
// Version 3: the header carries a content CRC and the table is rebuilt serially.
// The bump itself matters - it drops every cache built by the racy parallel path,
// which is how a one-time parse glitch became permanent mojibake for a player.
// Version 4: the engine's own file folds in first, as defaults.
// Version 5, with modules only: the ids a module defines again over another module's ride after the
// table, so a start that reads the cache says them too; a file the game makes for a module folds in with
// that module. A game without modules keeps version 4, and its cache, byte for byte.
constexpr u32 STRING_TABLE_CACHE_VERSION = 4;
constexpr u32 STRING_TABLE_CACHE_VERSION_MODULES = 5;
u32 CacheVersion() { return XMS::Active() ? STRING_TABLE_CACHE_VERSION_MODULES : STRING_TABLE_CACHE_VERSION; }
constexpr u32 MAX_CACHED_STRINGS = 1'000'000;
constexpr size_t MAX_CACHED_STRING_LENGTH = 1024 * 1024;
constexpr size_t STRING_TABLE_CACHE_HEADER_SIZE = sizeof(u32) * 5;
// log lines per pair of modules; the ledger and xms_report.json carry every id
constexpr u32 STRING_CLASH_LINES = 16;

struct LocalizationFile
{
    xr_string name;
    STRING_TABLE_MAP strings;
    u16 layer{0}; // the module that serves the file; 0 for the game's
};

// One name, two modules: the ledger row and a log line, each said once per start however often the table
// is read (a language switch reads it again).
void ReportClashes(const STRING_TABLE_CLASHES& clashes)
{
    if (clashes.empty())
        return;
    static xr_set<xr_string> said;
    struct Pair
    {
        shared_str loser, winner;
        u32 count{0};
    };
    xr_vector<Pair> pairs;
    bool added = false;
    for (const STRING_TABLE_CLASH& clash : clashes)
    {
        xr_string key = xr_string(clash.id.c_str()) + '\n' + clash.loser.c_str() + '\n' + clash.winner.c_str();
        if (!said.insert(std::move(key)).second)
            continue;
        const xr_string loser = xr_string(clash.loser.c_str()) + " (" + clash.loser_file.c_str() + ")";
        const xr_string winner = xr_string(clash.winner.c_str()) + " (" + clash.winner_file.c_str() + ")";
        XMS::AddConflict(XMS::ConflictKind::Name, (xr_string("string ") + clash.id.c_str()).c_str(), loser.c_str(),
            winner.c_str());
        added = true;
        auto pair = std::find_if(pairs.begin(), pairs.end(),
            [&clash](const Pair& p) { return p.loser == clash.loser && p.winner == clash.winner; });
        if (pair == pairs.end())
        {
            pairs.push_back(Pair{clash.loser, clash.winner});
            pair = pairs.end() - 1;
        }
        if (++pair->count <= STRING_CLASH_LINES)
            Msg("! XMS: string [%s] of module [%s] (%s) is defined again by module [%s] (%s), later in the load order"
                " - its text is the one shown", clash.id.c_str(), clash.loser.c_str(), clash.loser_file.c_str(),
                clash.winner.c_str(), clash.winner_file.c_str());
    }
    for (const Pair& pair : pairs)
        if (pair.count > STRING_CLASH_LINES)
            Msg("! XMS: ... and %u more string id(s) of module [%s] defined again by module [%s] (xms_conflicts)",
                pair.count - STRING_CLASH_LINES, pair.loser.c_str(), pair.winner.c_str());
    if (added)
        XMS::WriteReport();
}

u32 CalculateCacheSignature(const FS_FileSet& files, pcstr language)
{
    const u32 version = CacheVersion();
    u32 signature = crc32(&version, sizeof(version));
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
    // module set participates in the cache identity: same files, different
    // module order = different merged table
    const u32 signature = CalculateCacheSignature(fset, pData->m_sLanguage.c_str()) ^ XMS::CompositionHash();

    STRING_TABLE_CLASHES clashes;
    if (!LoadCache(cachePath, signature, pData->m_StringTable, clashes))
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

        // The engine's own strings are defaults: every file of the game folds in after them and
        // overrides what it defines, however the names sort.
        std::stable_partition(files.begin(), files.end(), [](const LocalizationFile& f)
        {
            return is_engine_xml(f.name.c_str());
        });

        // fold order: base layer first, then modules by load order; overrides
        // land by layer priority instead of bare filename alphabetics. A file the
        // game makes for a module (its characters', its items' strings) is that module's.
        if (XMS::Active())
        {
            for (LocalizationFile& f : files)
            {
                string_path full;
                xr_sprintf(full, "text" DELIMITER "%s" DELIMITER "%s", pData->m_sLanguage.c_str(), f.name.c_str());
                string_path resolved;
                FS.update_path(resolved, "$game_config$", full);
                xr_strlwr(resolved);
                if (const auto* desc = FS.GetFileDesc(resolved))
                    f.layer = XMS::OwnerLayer(desc->name);
            }
            std::stable_sort(files.begin(), files.end(),
                [](const LocalizationFile& a, const LocalizationFile& b) { return a.layer < b.layer; });
        }

        // Serial on purpose: this path runs only on a cache miss, and whatever it
        // produces is frozen into the cache for every later session. A parallel parse
        // that goes wrong once turns into permanently garbled text on that machine.
        for (LocalizationFile& file : files)
            Load(file.name.c_str(), file.strings);

        // the file of a module that defined an id last, for the ids modules define: one module's id that a
        // module later in the load order defines again is one name taken by two
        xr_flat_hash_map<shared_str, std::pair<size_t, unsigned>> module_owner;
        for (size_t f = 0; f < files.size(); ++f)
        {
            const LocalizationFile& file = files[f];
            for (const auto& [id, value] : file.strings)
            {
                if (const size_t over = XMS::Names::Redefines(module_owner, id, f, file.layer); over != XMS::Names::None)
                {
                    const LocalizationFile& was = files[over];
                    clashes.push_back({id, XMS::OwnerName(XMS::ModuleOfLayer(was.layer)), was.name.c_str(),
                        XMS::OwnerName(XMS::ModuleOfLayer(file.layer)), file.name.c_str()});
                }
#ifndef MASTER_GOLD
                if (pData->m_StringTable.find(id) != pData->m_StringTable.end())
                    Msg("* String table override [%s]", id.c_str());
#endif
                pData->m_StringTable[id] = value;
            }
        }
        SaveCache(cachePath, signature, pData->m_StringTable, clashes);
    }
    ReportClashes(clashes);

    if (!translate("st_currency", pData->m_sCurrency) &&
        !translate("ui_st_money_descr", pData->m_sCurrency) && // OGSR
        !translate("ui_st_money_regional", pData->m_sCurrency)) // xp-dev
    {
        pData->m_sCurrency = pSettingsGrizzly->read_if_exists<pcstr>("gameplay", "currency", "RU");
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

            // Skip folder that carries nothing but the engine's own string file
            // It's important to have 'else if' instead of simple 'if'
            else if (files->size() == 1 && is_engine_xml(files->at(0)))
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

bool CStringTable::LoadCache(pcstr cachePath, u32 signature, STRING_TABLE_MAP& strings, STRING_TABLE_CLASHES& clashes)
{
    IReader* reader = FS.r_open(cachePath);
    if (!reader)
        return false;

    const u32 version = CacheVersion();
    STRING_TABLE_MAP cachedStrings;
    STRING_TABLE_CLASHES cachedClashes;
    bool valid = reader->elapsed() >= static_cast<intptr_t>(STRING_TABLE_CACHE_HEADER_SIZE);
    u32 count = 0;
    u32 contentCrc = 0;
    if (valid)
    {
        valid = reader->r_u32() == STRING_TABLE_CACHE_MAGIC;
        valid = valid && reader->r_u32() == version;
        valid = valid && reader->r_u32() == signature;
        contentCrc = reader->r_u32();
        // The header signature only proves the source files did not change; the CRC
        // covers the payload itself, so a torn or bit-rotted cache rebuilds instead
        // of feeding stable garbage into every session from now on.
        valid = valid && crc32(reader->pointer(), reader->elapsed()) == contentCrc;
        count = valid ? reader->r_u32() : 0;
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
    if (valid && version == STRING_TABLE_CACHE_VERSION_MODULES)
    {
        const u32 clashCount = reader->elapsed() >= static_cast<intptr_t>(sizeof(u32)) ? reader->r_u32() : u32(-1);
        valid = clashCount <= MAX_CACHED_STRINGS;
        for (u32 i = 0; valid && i < clashCount; ++i)
        {
            xr_string fields[5];
            for (xr_string& field : fields)
                valid = valid && ReadCachedString(*reader, field);
            if (valid)
                cachedClashes.push_back({fields[0].c_str(), fields[1].c_str(), fields[2].c_str(), fields[3].c_str(),
                    fields[4].c_str()});
        }
    }
    valid = valid && reader->eof();
    FS.r_close(reader);

    if (!valid)
        return false;

    strings.swap(cachedStrings);
    clashes.swap(cachedClashes);
    // Content CRC in the line on purpose: a "symbols instead of letters" report can be
    // matched against a clean rebuild of the same content by this value alone.
    Msg("StringTable: loaded cache with %u strings, content crc[%08X]", count, contentCrc);
    return true;
}

void CStringTable::SaveCache(pcstr cachePath, u32 signature, const STRING_TABLE_MAP& strings,
    const STRING_TABLE_CLASHES& clashes)
{
    const u32 version = CacheVersion();
    CMemoryWriter payload;
    payload.w_u32(static_cast<u32>(strings.size()));
    for (const auto& [id, value] : strings)
    {
        payload.w_stringZ(id);
        payload.w_stringZ(value);
    }
    if (version == STRING_TABLE_CACHE_VERSION_MODULES)
    {
        payload.w_u32(static_cast<u32>(clashes.size()));
        for (const STRING_TABLE_CLASH& clash : clashes)
            for (const shared_str& field : {clash.id, clash.loser, clash.loser_file, clash.winner, clash.winner_file})
                payload.w_stringZ(field);
    }
    const u32 contentCrc = crc32(payload.pointer(), payload.size());
    Msg("StringTable: rebuilt %zu strings, content crc[%08X]", strings.size(), contentCrc);

    CMemoryWriter writer;
    writer.w_u32(STRING_TABLE_CACHE_MAGIC);
    writer.w_u32(version);
    writer.w_u32(signature);
    writer.w_u32(contentCrc);
    writer.w(payload.pointer(), payload.size());

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
