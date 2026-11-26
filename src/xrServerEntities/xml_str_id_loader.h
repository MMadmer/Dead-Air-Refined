#pragma once

#include "Common/object_broker.h"

#include "xrCommon/xr_hash_map.h"
#include "xrCore/XMS/xms_core.h"
#include "xrUICore/XML/xrUIXmlParser.h"

// T_INIT -  класс где определена статическая InitXmlIdToIndex
//          функция инициализации file_str и tag_name

//структура хранит строковый id элемента
//файл и позицию, где этот элемент находится
struct ITEM_DATA
{
    shared_str id;
    int index;
    int pos_in_file;
    CUIXml* _xml;
};
typedef xr_vector<ITEM_DATA> T_VECTOR;

void _destroy_item_data_vector_cont(T_VECTOR* vec);

// XMS: an id of one of these lists is one name for the whole game. When a module's file is one side of a
// duplicate, both owners are said - the first file read keeps the id, as it always did. False when no
// module is involved: the caller says it the old way.
inline bool xms_duplicate_id(pcstr tag, pcstr id, pcstr kept_file, pcstr dropped_file)
{
    if (!XMS::Active())
        return false;
    // what a module wrote has no length limit: no fixed buffer
    const auto owner = [](pcstr file)
    {
        const xr_string relative = xr_string("gameplay" DELIMITER) + file;
        return XMS::FileOwner(CONFIG_PATH, relative.c_str());
    };
    const XMS::Module* kept = owner(kept_file);
    const XMS::Module* dropped = owner(dropped_file);
    if (!kept && !dropped)
        return false;
    Msg("! XMS: %s [%s] of %s (%s) is defined again by %s (%s) - the first one keeps the id", tag, id,
        XMS::OwnerName(kept), kept_file, XMS::OwnerName(dropped), dropped_file);
    const xr_string subject = xr_string(tag) + " " + id;
    const xr_string loser = xr_string(XMS::OwnerName(dropped)) + " (" + dropped_file + ")";
    const xr_string winner = xr_string(XMS::OwnerName(kept)) + " (" + kept_file + ")";
    XMS::AddConflict(XMS::ConflictKind::Name, subject.c_str(), loser.c_str(), winner.c_str());
    return true;
}

#define TEMPLATE_SPECIALIZATION template <typename T_INIT>
#define CSXML_IdToIndex CXML_IdToIndex<T_INIT>

TEMPLATE_SPECIALIZATION
class CXML_IdToIndex
{
private:
    static T_VECTOR* m_pItemDataVector;

protected:
    CXML_IdToIndex() = default;

    //имена xml файлов (разделенных запятой) из которых
    //производить загрузку элементов
    static LPCSTR file_str;
    //имена тегов
    static LPCSTR tag_name;

public:
    static void InitInternal(bool crashOnFail = true, bool ignoreMissingEndTagError = false);

    static const ITEM_DATA* GetById(const shared_str& str_id, bool no_assert = false);
    static const ITEM_DATA* GetByIndex(int index, bool no_assert = false);

    static int IdToIndex(const shared_str& str_id, int default_index = -1, bool no_assert = false)
    {
        const ITEM_DATA* item = GetById(str_id, no_assert);
        return item ? item->index : default_index;
    }
    static shared_str IndexToId(int index, shared_str default_id = nullptr, bool no_assert = false)
    {
        const ITEM_DATA* item = GetByIndex(index, no_assert);
        return item ? item->id : default_id;
    }

    static int GetMaxIndex() { return m_pItemDataVector->size() - 1; }
    //удаление статичекого массива
    static void DeleteIdToIndexData();
};

TEMPLATE_SPECIALIZATION
T_VECTOR* CSXML_IdToIndex::m_pItemDataVector = nullptr;

TEMPLATE_SPECIALIZATION
LPCSTR CSXML_IdToIndex::file_str = nullptr;
TEMPLATE_SPECIALIZATION
LPCSTR CSXML_IdToIndex::tag_name = nullptr;

TEMPLATE_SPECIALIZATION
const ITEM_DATA* CSXML_IdToIndex::GetById(const shared_str& str_id, bool no_assert)
{
    T_INIT::InitXmlIdToIndex();
    T_VECTOR::iterator it;
    for (it = m_pItemDataVector->begin(); m_pItemDataVector->end() != it; ++it)
    {
        if ((*it).id == str_id)
            break;
    }

    if (it == m_pItemDataVector->end())
    {
#if 0 // ndef MASTER_GOLD
        int i = 0;
        for (it = m_pItemDataVector->begin(); m_pItemDataVector->end() != it; ++it, i++)
            Msg("[%d]=[%s]", i, *(*it).id);
#endif

        R_ASSERT3(no_assert, "item not found, id", str_id.c_str());
        return nullptr;
    }

    return &(*it);
}

TEMPLATE_SPECIALIZATION
const ITEM_DATA* CSXML_IdToIndex::GetByIndex(int index, bool no_assert)
{
    if ((size_t)index >= m_pItemDataVector->size())
    {
        R_ASSERT3(no_assert, "item by index not found in files", file_str);
        return nullptr;
    }
    return &(*m_pItemDataVector)[index];
}

TEMPLATE_SPECIALIZATION
void CSXML_IdToIndex::DeleteIdToIndexData()
{
    VERIFY(m_pItemDataVector);
    _destroy_item_data_vector_cont(m_pItemDataVector);

    xr_delete(m_pItemDataVector);
}

TEMPLATE_SPECIALIZATION
void CSXML_IdToIndex::InitInternal(bool crashOnFail /*= true*/, bool ignoreMissingEndTagError /*= false*/)
{
    VERIFY(!m_pItemDataVector);
    T_INIT::InitXmlIdToIndex();

    m_pItemDataVector = xr_new<T_VECTOR>();

    VERIFY(file_str);
    R_ASSERT2_CURE(tag_name, "XML tag name is missing", { return; });

    string_path xml_file;
    int count = _GetItemCount(file_str);
    int index = 0;
    // an id -> the file that gave it, to name both files of a duplicate
    xr_flat_hash_map<shared_str, u32> knownIds;
    xr_vector<xr_string> files_read(size_t(std::max(count, 0)));
    bool reported = false;
    for (int it = 0; it < count; ++it)
    {
        _GetItem(file_str, it, xml_file);

        CUIXml* uiXml = xr_new<CUIXml>();
        xr_string& xml_file_full = files_read[it];
        xml_file_full = xml_file;
        xml_file_full += ".xml";

        uiXml->IgnoreMissingEndTagError(ignoreMissingEndTagError);
        const bool success = uiXml->Load(CONFIG_PATH, "gameplay", xml_file_full.c_str(), crashOnFail);
        if (!success)
        {
            delete_data(uiXml);
            continue;
        }

        //общий список
        int items_num = uiXml->GetNodesNum(uiXml->GetRoot(), tag_name);

        for (int i = 0; i < items_num; ++i)
        {
            LPCSTR item_name = uiXml->ReadAttrib(uiXml->GetRoot(), tag_name, i, "id", nullptr);
            if (!item_name || !item_name[0])
            {
                continue;
            }

            const shared_str itemId = item_name;
            const auto [known, fresh] = knownIds.emplace(itemId, u32(it));
            if (!fresh)
            {
                if (xms_duplicate_id(tag_name, item_name, files_read[known->second].c_str(), xml_file_full.c_str()))
                    reported = true;
                else
                {
#ifndef MASTER_GOLD
                    Msg("! duplicate %s with id[%s] in %s", tag_name, item_name, xml_file_full.c_str());
#endif
                }
                continue;
            }

            ITEM_DATA data;
            data.id = itemId;
            data.index = index;
            data.pos_in_file = i;
            //.				data.file_name		= xml_file;
            data._xml = uiXml;
            m_pItemDataVector->push_back(data);

            index++;
        }
        if (0 == items_num)
            delete_data(uiXml);
    }
    // the lists are read when the game first asks for one, after the report was written
    if (reported)
        XMS::WriteReport();
}

#undef TEMPLATE_SPECIALIZATION
