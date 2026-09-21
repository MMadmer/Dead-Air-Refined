////////////////////////////////////////////////////////////////////////////
//  Module      : vertex_manager_fixed_inline.h
//  Created     : 21.03.2002
//  Modified    : 01.03.2004
//  Author      : Dmitriy Iassenev
//  Description : Fixed vertex manager inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#define TEMPLATE_SPECIALIZATION                           \
    template <typename TPathId, typename TIndex, u8 Mask> \
    template <typename TPathBuilder, typename TVertexAllocator, typename TCompoundVertex>

#define CFixedVertexManager \
    CVertexManagerFixed<TPathId, TIndex, Mask>::CDataStorage<TPathBuilder, TVertexAllocator, TCompoundVertex>

TEMPLATE_SPECIALIZATION
inline CFixedVertexManager::CDataStorage(const u32 vertex_count)
    : CDataStorageBase(vertex_count), CDataStorageAllocator()
{
    m_current_path_id = TPathId(0);
    m_max_node_count = vertex_count;
    m_path_ids = xr_alloc<TPathId>(vertex_count);
    m_vertices = xr_alloc<TCompoundVertex*>(vertex_count);
    ZeroMemory(m_path_ids, vertex_count * sizeof(TPathId));
    ZeroMemory(m_vertices, vertex_count * sizeof(TCompoundVertex*));
}

TEMPLATE_SPECIALIZATION
CFixedVertexManager::~CDataStorage()
{
    xr_free(m_path_ids);
    xr_free(m_vertices);
}
TEMPLATE_SPECIALIZATION
inline void CFixedVertexManager::init()
{
    CDataStorageBase::init();
    CDataStorageAllocator::init();
    ++m_current_path_id;
    if (!m_current_path_id)
    {
        ZeroMemory(m_path_ids, m_max_node_count * sizeof(TPathId));
        ++m_current_path_id;
    }
}

TEMPLATE_SPECIALIZATION
inline bool CFixedVertexManager::is_opened(const TCompoundVertex& vertex) const { return !!vertex.opened(); }
TEMPLATE_SPECIALIZATION
inline bool CFixedVertexManager::is_visited(const TIndex& vertex_id) const
{
    VERIFY(vertex_id < m_max_node_count);
    return m_path_ids[vertex_id] == m_current_path_id;
}

TEMPLATE_SPECIALIZATION
inline bool CFixedVertexManager::is_closed(const TCompoundVertex& vertex) const
{
    return is_visited(vertex) && !is_opened(vertex);
}

TEMPLATE_SPECIALIZATION
inline TCompoundVertex& CFixedVertexManager::get_node(const TIndex& vertex_id) const
{
    VERIFY(vertex_id < m_max_node_count);
    VERIFY(is_visited(vertex_id));
    return *m_vertices[vertex_id];
}

TEMPLATE_SPECIALIZATION
inline TCompoundVertex* CFixedVertexManager::find_vertex(const TIndex& vertex_id) const
{
    VERIFY(vertex_id < m_max_node_count);
    return m_path_ids[vertex_id] == m_current_path_id ? m_vertices[vertex_id] : nullptr;
}

TEMPLATE_SPECIALIZATION
inline TCompoundVertex& CFixedVertexManager::create_vertex(TCompoundVertex& vertex, const TIndex& vertex_id)
{
    VERIFY(vertex_id < m_max_node_count);
    m_vertices[vertex_id] = &vertex;
    m_path_ids[vertex_id] = m_current_path_id;
    vertex._index = vertex_id;
    return vertex;
}

TEMPLATE_SPECIALIZATION
inline void CFixedVertexManager::add_opened(TCompoundVertex& vertex) { vertex._opened = true; }
TEMPLATE_SPECIALIZATION
inline void CFixedVertexManager::add_closed(TCompoundVertex& vertex) { vertex._opened = false; }
TEMPLATE_SPECIALIZATION
inline TPathId CFixedVertexManager::current_path_id() const { return m_current_path_id; }
#undef TEMPLATE_SPECIALIZATION
#undef CFixedVertexManager
