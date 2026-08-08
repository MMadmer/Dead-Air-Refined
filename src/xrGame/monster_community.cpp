//////////////////////////////////////////////////////////////////////////
// monster_community.cpp: структура представления группировки для монстров
//
//////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "monster_community.h"

#define MONSTER_RELATIONS_SECT "monster_communities"
#define MONSTER_COMMUNITIES "communities"
#define MONSTER_RELATIONS_TABLE "monster_relations"

//////////////////////////////////////////////////////////////////////////
MONSTER_COMMUNITY_DATA::MONSTER_COMMUNITY_DATA(MONSTER_COMMUNITY_INDEX idx, MONSTER_COMMUNITY_ID idn, LPCSTR team_str)
{
    index = idx;
    id = idn;
    team = (u8)atoi(team_str);
}

//////////////////////////////////////////////////////////////////////////
MONSTER_COMMUNITY::MONSTER_RELATION_TABLE MONSTER_COMMUNITY::m_relation_table;

//////////////////////////////////////////////////////////////////////////
MONSTER_COMMUNITY::MONSTER_COMMUNITY() { m_current_index = NO_MONSTER_COMMUNITY_INDEX; }
MONSTER_COMMUNITY::~MONSTER_COMMUNITY() {}

// A species typo in a creature section used to be fatal: GetById asserted with the raw id,
// taking the whole session down for one bad config line. Keep the documented "no community"
// state instead and report the id once, so addon authors can fix the source data. Consumers
// must check index() against NO_MONSTER_COMMUNITY_INDEX (see CEntity::Load) and the
// accessors below stay in bounds for the unset state.
void MONSTER_COMMUNITY::set(MONSTER_COMMUNITY_ID id)
{
    m_current_index = IdToIndex(id, NO_MONSTER_COMMUNITY_INDEX, true);
    if (m_current_index != NO_MONSTER_COMMUNITY_INDEX)
        return;

    static xr_vector<shared_str> reported;
    if (reported.size() < 64 && std::find(reported.begin(), reported.end(), id) == reported.end())
    {
        reported.push_back(id);
        Msg("! Unknown monster community [%s]: not present in [%s], usually a missing or "
            "misspelled 'species' line in the creature section",
            id.c_str(), MONSTER_RELATIONS_SECT);
    }
}

void MONSTER_COMMUNITY::set(MONSTER_COMMUNITY_INDEX index) { m_current_index = index; }
MONSTER_COMMUNITY_ID MONSTER_COMMUNITY::id() const { return IndexToId(m_current_index, nullptr, true); }
MONSTER_COMMUNITY_INDEX MONSTER_COMMUNITY::index() const { return m_current_index; }

u8 MONSTER_COMMUNITY::team() const
{
    VERIFY(m_current_index != NO_MONSTER_COMMUNITY_INDEX);
    if (m_current_index < 0 || size_t(m_current_index) >= m_pItemDataVector->size())
        return 0;
    return (*m_pItemDataVector)[m_current_index].team;
}
void MONSTER_COMMUNITY::InitIdToIndex()
{
    section_name = MONSTER_RELATIONS_SECT;
    line_name = MONSTER_COMMUNITIES;
    m_relation_table.set_table_params(MONSTER_RELATIONS_TABLE);
}

int MONSTER_COMMUNITY::relation(MONSTER_COMMUNITY_INDEX to) { return relation(m_current_index, to); }
int MONSTER_COMMUNITY::relation(MONSTER_COMMUNITY_INDEX from, MONSTER_COMMUNITY_INDEX to)
{
    VERIFY(from >= 0 && from < (int)m_relation_table.table().size());
    VERIFY(to >= 0 && to < (int)m_relation_table.table().size());

    // The unset community (unknown species survives with NO_MONSTER_COMMUNITY_INDEX) is
    // neutral to everything; the VERIFYs above vanish in release and -1 would index OOB.
    if (from < 0 || from >= (int)m_relation_table.table().size() ||
        to < 0 || to >= (int)m_relation_table.table().size())
        return 0;

    return m_relation_table.table()[from][to];
}

void MONSTER_COMMUNITY::DeleteIdToIndexData()
{
    m_relation_table.clear();
    inherited::DeleteIdToIndexData();
}
